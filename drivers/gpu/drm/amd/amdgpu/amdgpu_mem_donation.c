// SPDX-License-Identifier: MIT
/*
 * Reversible donation of an APU's firmware carveout to System RAM.
 *
 * The GPU owns the complete carveout at probe.  A donation transaction first
 * evicts movable VRAM occupants and pins an exact-placement guard BO, removes
 * the conflicting WC aperture alias, and then hot-adds the corresponding CPU
 * physical memory block to ZONE_MOVABLE.  Return performs those steps in the
 * opposite order and does not release the guard unless hotremove succeeds.
 */

#include <linux/io.h>
#include <linux/cc_platform.h>
#include <linux/delay.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/module.h>
#include <linux/numa.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "amdgpu.h"
#include "amdgpu_mem_donation.h"
#include "amdgpu_object.h"
#include "amdgpu_vram_mgr.h"

#if defined(CONFIG_X86_64) && defined(CONFIG_MEMORY_HOTPLUG) && \
	defined(CONFIG_MEMORY_HOTREMOVE)

#include <asm/set_memory.h>

#define AMDGPU_DONATED_RESOURCE_NAME "System RAM (amdgpu)"

#define AMDGPU_MEM_DONATION_EVENT(action) BIT(action)

static bool
amdgpu_mem_donation_overlaps(struct amdgpu_mem_donation *donation,
			     unsigned long start_pfn,
			     unsigned long nr_pages)
{
	u64 event_start = PFN_PHYS(start_pfn);
	u64 event_size = PFN_PHYS(nr_pages);
	u64 donation_end, event_end;

	if (check_add_overflow(donation->range_phys_start,
			       donation->range_size, &donation_end) ||
	    check_add_overflow(event_start, event_size, &event_end))
		return true;

	return event_start < donation_end &&
	       donation->range_phys_start < event_end;
}

static int
amdgpu_mem_donation_memory_notify(struct notifier_block *nb,
				  unsigned long action, void *arg)
{
	struct amdgpu_mem_donation *donation =
		container_of(nb, struct amdgpu_mem_donation, memory_nb);
	struct memory_notify *memory = arg;
	struct task_struct *hotplug_task;

	if (action != MEM_GOING_ONLINE && action != MEM_GOING_OFFLINE)
		return NOTIFY_DONE;
	if (!amdgpu_mem_donation_overlaps(donation, memory->start_pfn,
					  memory->nr_pages))
		return NOTIFY_DONE;

	/*
	 * The memory-hotplug helpers invoke the blocking notifier synchronously.
	 * Qualifying the permit with current prevents another sysfs task from
	 * borrowing it while an AMDGPU transaction is between hotplug calls.
	 */
	hotplug_task = READ_ONCE(donation->hotplug_task);
	if (hotplug_task == current &&
	    (READ_ONCE(donation->hotplug_events) &
	     AMDGPU_MEM_DONATION_EVENT(action)))
		return NOTIFY_OK;

	return notifier_from_errno(-EBUSY);
}

static int
amdgpu_mem_donation_hotplug_begin(struct amdgpu_mem_donation *donation,
				  unsigned long events)
{
	if (WARN_ON_ONCE(READ_ONCE(donation->hotplug_task)))
		return -EBUSY;

	WRITE_ONCE(donation->hotplug_events, events);
	WRITE_ONCE(donation->hotplug_task, current);
	return 0;
}

static void
amdgpu_mem_donation_hotplug_end(struct amdgpu_mem_donation *donation)
{
	WARN_ON_ONCE(READ_ONCE(donation->hotplug_task) != current);
	WRITE_ONCE(donation->hotplug_task, NULL);
	WRITE_ONCE(donation->hotplug_events, 0);
}

static unsigned long amdgpu_mem_donation_virt(struct amdgpu_device *adev,
					      u64 vram_offset)
{
	return (unsigned long)adev->mman.aper_base_kaddr + vram_offset;
}

static int amdgpu_mem_donation_set_aperture_present(struct amdgpu_device *adev,
						    u64 vram_offset, u64 size,
						    bool present)
{
	unsigned long address = amdgpu_mem_donation_virt(adev, vram_offset);
	unsigned long pages = size >> PAGE_SHIFT;

	if (pages > INT_MAX)
		return -E2BIG;
	if (present)
		return set_memory_p(address, pages);
	return set_memory_np(address, pages);
}

static void amdgpu_mem_donation_release_wc(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;

	if (donation->wc_released)
		return;

	/* Invalidate every user aperture mapping before changing ownership. */
	unmap_mapping_range(adev->ddev.anon_inode->i_mapping, 0, 0, 1);
	arch_phys_wc_del(adev->gmc.vram_mtrr);
	arch_io_free_memtype_wc(adev->gmc.aper_base, adev->gmc.aper_size);
	donation->wc_released = true;
}

static int amdgpu_mem_donation_restore_wc(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	int r;

	if (!donation->wc_released)
		return 0;

	r = arch_io_reserve_memtype_wc(adev->gmc.aper_base,
				       adev->gmc.aper_size);
	if (r)
		return r;

	adev->gmc.vram_mtrr = arch_phys_wc_add(adev->gmc.aper_base,
					       adev->gmc.aper_size);
	donation->wc_released = false;
	return 0;
}

static int amdgpu_mem_donation_add_system_block(struct amdgpu_device *adev,
						u64 phys_start)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	unsigned long events = AMDGPU_MEM_DONATION_EVENT(MEM_GOING_ONLINE);
	int nid = memory_add_physaddr_to_nid(phys_start);
	int r;

	if (nid < 0)
		nid = dev_to_node(adev->dev);
	if (nid < 0)
		nid = numa_node_id();

	r = amdgpu_mem_donation_hotplug_begin(donation, events);
	if (r)
		return r;

	r = __add_memory_driver_managed(nid, phys_start, donation->block_size,
					AMDGPU_DONATED_RESOURCE_NAME,
					MHP_MERGE_RESOURCE |
					MHP_ONLINE_REQUIRED,
					MMOP_ONLINE_MOVABLE);
	amdgpu_mem_donation_hotplug_end(donation);
	return r;
}

static void amdgpu_mem_donation_free_guard(struct amdgpu_device *adev,
					   unsigned int index)
{
	amdgpu_bo_free_kernel(&adev->mem_donation.guards[index], NULL, NULL);
}

static unsigned int
amdgpu_mem_donation_first_active(struct amdgpu_mem_donation *donation)
{
	return donation->nr_blocks - donation->active_blocks;
}

static void amdgpu_mem_donation_put_module(struct amdgpu_mem_donation *donation)
{
	if (!donation->active_blocks)
		module_put(THIS_MODULE);
}

static int amdgpu_mem_donation_quarantine_to_system(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	unsigned int index;
	u64 phys_start, vram_offset;
	int r;

	if (donation->boundary_state == AMDGPU_MEM_DONATION_BOUNDARY_NONE)
		return -EINVAL;

	index = amdgpu_mem_donation_first_active(donation);
	vram_offset = donation->range_vram_start +
			      (u64)index * donation->block_size;
	phys_start = donation->range_phys_start +
			     (u64)index * donation->block_size;

	/* Repeating this operation completes any earlier partial CPA change. */
	r = amdgpu_mem_donation_set_aperture_present(adev, vram_offset,
						     donation->block_size,
						     false);
	if (r) {
		donation->boundary_state = AMDGPU_MEM_DONATION_BOUNDARY_UNKNOWN;
		return r;
	}
	donation->boundary_state = AMDGPU_MEM_DONATION_BOUNDARY_NONPRESENT;

	r = amdgpu_mem_donation_add_system_block(adev, phys_start);
	if (r)
		return r;

	donation->donated_size += donation->block_size;
	donation->boundary_state = AMDGPU_MEM_DONATION_BOUNDARY_NONE;
	return 0;
}

static int amdgpu_mem_donation_quarantine_to_gpu(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	unsigned int index;
	u64 vram_offset;
	int r;

	if (donation->boundary_state == AMDGPU_MEM_DONATION_BOUNDARY_NONE)
		return -EINVAL;

	index = amdgpu_mem_donation_first_active(donation);
	vram_offset = donation->range_vram_start +
			      (u64)index * donation->block_size;
	r = amdgpu_mem_donation_set_aperture_present(adev, vram_offset,
						     donation->block_size,
						     true);
	if (r) {
		donation->boundary_state = AMDGPU_MEM_DONATION_BOUNDARY_UNKNOWN;
		return r;
	}
	donation->boundary_state = AMDGPU_MEM_DONATION_BOUNDARY_PRESENT;

	/* Keep the last guard until the aperture's WC state is restored. */
	if (donation->active_blocks == 1) {
		r = amdgpu_mem_donation_restore_wc(adev);
		if (r)
			return r;
	}

	amdgpu_mem_donation_free_guard(adev, index);
	donation->active_blocks--;
	donation->boundary_state = AMDGPU_MEM_DONATION_BOUNDARY_NONE;
	amdgpu_mem_donation_put_module(donation);
	return 0;
}

static int amdgpu_mem_donation_add_block(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	struct amdgpu_bo *guard = NULL;
	unsigned int index;
	u64 vram_offset;
	int r;

	if (donation->boundary_state != AMDGPU_MEM_DONATION_BOUNDARY_NONE)
		return -EINVAL;
	if (donation->active_blocks == donation->nr_blocks)
		return -ENOSPC;

	index = donation->nr_blocks - donation->active_blocks - 1;
	vram_offset = donation->range_vram_start +
			      (u64)index * donation->block_size;
	r = amdgpu_bo_create_kernel_at_evict(adev, vram_offset,
					     donation->block_size, &guard);
	if (r)
		return r;

	if (!donation->active_blocks) {
		if (!try_module_get(THIS_MODULE)) {
			amdgpu_bo_free_kernel(&guard, NULL, NULL);
			return -ENODEV;
		}
		amdgpu_mem_donation_release_wc(adev);
	}

	/*
	 * Publish quarantine before changing mappings.  Any later failure keeps
	 * the exact guard and module reference until a subsequent write repairs
	 * the block toward either System RAM or GPU ownership.
	 */
	donation->guards[index] = guard;
	donation->boundary_state = AMDGPU_MEM_DONATION_BOUNDARY_PRESENT;
	donation->active_blocks++;

	return amdgpu_mem_donation_quarantine_to_system(adev);
}

static int amdgpu_mem_donation_return_block(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	unsigned long events =
		AMDGPU_MEM_DONATION_EVENT(MEM_GOING_OFFLINE) |
		AMDGPU_MEM_DONATION_EVENT(MEM_GOING_ONLINE);
	unsigned int index;
	u64 phys_start;
	int r;

	if (donation->boundary_state != AMDGPU_MEM_DONATION_BOUNDARY_NONE)
		return -EINVAL;

	index = amdgpu_mem_donation_first_active(donation);
	phys_start = donation->range_phys_start +
			     (u64)index * donation->block_size;
	r = amdgpu_mem_donation_hotplug_begin(donation, events);
	if (r)
		return r;

	r = offline_and_remove_memory(phys_start, donation->block_size);
	amdgpu_mem_donation_hotplug_end(donation);
	if (r)
		return r;

	donation->donated_size -= donation->block_size;
	donation->boundary_state = AMDGPU_MEM_DONATION_BOUNDARY_NONPRESENT;
	return amdgpu_mem_donation_quarantine_to_gpu(adev);
}

static int
amdgpu_mem_donation_set_locked(struct amdgpu_device *adev, u64 size)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	int r;

	/* Repair the only possible quarantined block at the active boundary. */
	if (donation->boundary_state != AMDGPU_MEM_DONATION_BOUNDARY_NONE) {
		if (size > donation->donated_size)
			r = amdgpu_mem_donation_quarantine_to_system(adev);
		else
			r = amdgpu_mem_donation_quarantine_to_gpu(adev);
		if (r)
			return r;
	}

	while (donation->donated_size < size) {
		r = amdgpu_mem_donation_add_block(adev);
		if (r)
			return r;
	}
	while (donation->donated_size > size) {
		r = amdgpu_mem_donation_return_block(adev);
		if (r)
			return r;
	}
	return 0;
}

int amdgpu_mem_donation_set_size(struct amdgpu_device *adev, u64 size)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	int r;

	if (!donation->supported)
		return -EOPNOTSUPP;
	if (size > donation->range_size ||
	    !IS_ALIGNED(size, donation->block_size))
		return -EINVAL;

	mutex_lock(&donation->lock);
	if (!donation->supported)
		r = -EOPNOTSUPP;
	else if (donation->lifecycle != AMDGPU_MEM_DONATION_LIFECYCLE_NONE)
		r = -EBUSY;
	else
		r = amdgpu_mem_donation_set_locked(adev, size);
	mutex_unlock(&donation->lock);
	return r;
}

u64 amdgpu_mem_donation_get_size(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	u64 size = 0;

	if (!donation->supported)
		return 0;
	mutex_lock(&donation->lock);
	if (donation->supported)
		size = donation->donated_size;
	mutex_unlock(&donation->lock);
	return size;
}

u64 amdgpu_mem_donation_get_quarantined_size(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	u64 size = 0;

	if (!donation->supported)
		return 0;
	mutex_lock(&donation->lock);
	if (donation->supported &&
	    donation->boundary_state != AMDGPU_MEM_DONATION_BOUNDARY_NONE)
		size = donation->block_size;
	mutex_unlock(&donation->lock);
	return size;
}

int
amdgpu_mem_donation_begin(struct amdgpu_device *adev,
			  enum amdgpu_mem_donation_lifecycle lifecycle,
			  bool rollback)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	int r, rollback_r;

	if (lifecycle == AMDGPU_MEM_DONATION_LIFECYCLE_NONE)
		return -EINVAL;
	if (!donation->supported)
		return 0;

	mutex_lock(&donation->lock);
	if (!donation->supported) {
		r = 0;
		goto out_unlock;
	}
	if (donation->lifecycle != AMDGPU_MEM_DONATION_LIFECYCLE_NONE) {
		r = -EBUSY;
		goto out_unlock;
	}

	donation->lifecycle = lifecycle;
	donation->restore_size = donation->donated_size;
	r = amdgpu_mem_donation_set_locked(adev, 0);
	if (r) {
		if (rollback) {
			rollback_r =
				amdgpu_mem_donation_set_locked(adev, donation->restore_size);
			if (rollback_r)
				dev_crit(adev->dev,
					 "failed to restore donation after transition veto: %d\n",
					 rollback_r);
		}
		donation->restore_size = 0;
		donation->lifecycle = AMDGPU_MEM_DONATION_LIFECYCLE_NONE;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&donation->lock);
	return r;
}

enum amdgpu_mem_donation_work_op {
	AMDGPU_MEM_DONATION_WORK_SET_ZERO,
	AMDGPU_MEM_DONATION_WORK_TRANSITION_BEGIN,
};

struct amdgpu_mem_donation_work {
	struct work_struct work;
	struct amdgpu_device *adev;
	enum amdgpu_mem_donation_work_op op;
	int r;
};

static void amdgpu_mem_donation_work(struct work_struct *work)
{
	struct amdgpu_mem_donation_work *donation_work =
		container_of(work, struct amdgpu_mem_donation_work, work);

	if (donation_work->op == AMDGPU_MEM_DONATION_WORK_TRANSITION_BEGIN)
		donation_work->r =
			amdgpu_mem_donation_begin(donation_work->adev,
						  AMDGPU_MEM_DONATION_LIFECYCLE_SHUTDOWN,
						  false);
	else
		donation_work->r =
			amdgpu_mem_donation_set_size(donation_work->adev, 0);
}

static int
amdgpu_mem_donation_run_work(struct amdgpu_device *adev,
			     enum amdgpu_mem_donation_work_op op)
{
	struct amdgpu_mem_donation_work donation_work = {
		.adev = adev,
		.op = op,
	};

	INIT_WORK_ONSTACK(&donation_work.work, amdgpu_mem_donation_work);
	schedule_work(&donation_work.work);
	flush_work(&donation_work.work);
	destroy_work_on_stack(&donation_work.work);

	return donation_work.r;
}

int amdgpu_mem_donation_shutdown_begin(struct amdgpu_device *adev)
{
	/*
	 * Shutdown holds the device lock needed by later PCI error callbacks.
	 * Cancel a prepared AER transition before waiting for ownership.
	 */
	amdgpu_mem_donation_end(adev, AMDGPU_MEM_DONATION_LIFECYCLE_PCI, false);
	return amdgpu_mem_donation_run_work(adev,
		AMDGPU_MEM_DONATION_WORK_TRANSITION_BEGIN);
}

/*
 * A failed final WC reservation must not strand shutdown after hotremove has
 * already succeeded.  The exact-placement guard remains pinned, so the GPU
 * cannot reuse the range before poweroff.  Only accept this state when CPA
 * has positively restored the aperture PTEs; a failed or partial CPA change
 * still requires repair.
 */
bool amdgpu_mem_donation_shutdown_safe(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	bool safe = false;

	if (!donation->supported)
		return true;

	mutex_lock(&donation->lock);
	if (!donation->supported) {
		safe = true;
		goto out_unlock;
	}
	if (donation->lifecycle != AMDGPU_MEM_DONATION_LIFECYCLE_NONE ||
	    donation->donated_size || donation->active_blocks != 1)
		goto out_unlock;

	safe = donation->boundary_state == AMDGPU_MEM_DONATION_BOUNDARY_PRESENT;
	if (safe)
		donation->lifecycle = AMDGPU_MEM_DONATION_LIFECYCLE_SHUTDOWN;

out_unlock:
	mutex_unlock(&donation->lock);
	return safe;
}

void
amdgpu_mem_donation_end(struct amdgpu_device *adev,
			enum amdgpu_mem_donation_lifecycle lifecycle,
			bool restore)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	int r;

	if (!donation->supported)
		return;

	mutex_lock(&donation->lock);
	if (!donation->supported || donation->lifecycle != lifecycle)
		goto out_unlock;

	if (restore) {
		r = amdgpu_mem_donation_set_locked(adev, donation->restore_size);
		if (r)
			dev_err(adev->dev,
				"failed to restore %llu donated bytes after transition: %d\n",
				donation->restore_size, r);
	}
	donation->restore_size = 0;
	donation->lifecycle = AMDGPU_MEM_DONATION_LIFECYCLE_NONE;

out_unlock:
	mutex_unlock(&donation->lock);
}

int amdgpu_mem_donation_init(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	u64 phys_end, mappable_size, offset_start, offset_end;
	unsigned long address, pages;
	int r;

	if (!(adev->flags & AMD_IS_APU) || adev->gmc.is_app_apu ||
	    amdgpu_passthrough(adev) ||
	    adev->pm.rpm_mode != AMDGPU_RUNPM_NONE ||
	    adev->gmc.xgmi.connected_to_cpu || !adev->mman.aper_base_kaddr ||
	    cc_platform_has(CC_ATTR_HOST_MEM_ENCRYPT))
		return 0;

	donation->block_size = memory_block_size_bytes();
	mappable_size = min(adev->gmc.visible_vram_size,
			    adev->gmc.aper_size);
	if (!donation->block_size ||
	    check_add_overflow(adev->gmc.aper_base,
			       mappable_size, &phys_end))
		return 0;

	donation->range_phys_start = ALIGN(adev->gmc.aper_base,
					   donation->block_size);
	phys_end = round_down(phys_end, donation->block_size);
	if (donation->range_phys_start >= phys_end)
		return 0;

	offset_start = donation->range_phys_start - adev->gmc.aper_base;
	offset_end = phys_end - adev->gmc.aper_base;
	r = amdgpu_vram_mgr_find_donatable_range(&adev->mman.vram_mgr,
						 offset_start, offset_end,
						  donation->block_size,
						  &donation->range_vram_start,
						  &donation->range_size);
	if (r)
		return 0;

	donation->range_phys_start = adev->gmc.aper_base +
					     donation->range_vram_start;
	if (donation->range_size / donation->block_size > UINT_MAX)
		return 0;

	pages = donation->range_size >> PAGE_SHIFT;
	if (pages > INT_MAX)
		return 0;
	/* Pay any large-page split allocation cost before ownership changes. */
	address = amdgpu_mem_donation_virt(adev, donation->range_vram_start);
	r = set_memory_4k(address, pages);
	if (r)
		return 0;

	donation->nr_blocks = donation->range_size / donation->block_size;
	donation->guards = kcalloc(donation->nr_blocks,
				   sizeof(*donation->guards), GFP_KERNEL);
	if (!donation->guards)
		return -ENOMEM;

	mutex_init(&donation->lock);
	donation->memory_nb.notifier_call = amdgpu_mem_donation_memory_notify;
	r = register_memory_notifier(&donation->memory_nb);
	if (r) {
		mutex_destroy(&donation->lock);
		kfree(donation->guards);
		donation->guards = NULL;
		return r;
	}
	donation->supported = true;
	drm_info(adev_to_drm(adev),
		 "reversible memory donation: %llu blocks of %llu bytes at CPU %#llx, VRAM offset %#llx\n",
		 donation->range_size / donation->block_size,
		 donation->block_size, donation->range_phys_start,
		 donation->range_vram_start);
	return 0;
}

void amdgpu_mem_donation_fini(struct amdgpu_device *adev)
{
	struct amdgpu_mem_donation *donation = &adev->mem_donation;
	unsigned int index;
	int r;

	if (!donation->supported)
		return;

	/* No PM/reset transition may outlive device teardown. */
	mutex_lock(&donation->lock);
	donation->lifecycle = AMDGPU_MEM_DONATION_LIFECYCLE_NONE;
	donation->restore_size = 0;
	mutex_unlock(&donation->lock);

	/* The driver remove callback cannot return an error. */
	for (;;) {
		r = amdgpu_mem_donation_run_work(adev,
						 AMDGPU_MEM_DONATION_WORK_SET_ZERO);
		if (!r)
			break;

		mutex_lock(&donation->lock);
		/*
		 * Once hotremove and CPA restoration succeeded, an unrecoverable WC
		 * reservation failure need not strand unbind.  Sysfs is gone and the
		 * aperture is about to be unmapped, so release the last guard while
		 * retaining wc_released to suppress a nonexistent reservation free.
		 */
		if (!donation->donated_size && donation->active_blocks == 1 &&
		    donation->boundary_state ==
			    AMDGPU_MEM_DONATION_BOUNDARY_PRESENT) {
			index = amdgpu_mem_donation_first_active(donation);
			amdgpu_mem_donation_free_guard(adev, index);
			donation->active_blocks--;
			donation->boundary_state =
				AMDGPU_MEM_DONATION_BOUNDARY_NONE;
			amdgpu_mem_donation_put_module(donation);
			mutex_unlock(&donation->lock);
			dev_warn(adev->dev,
				 "cache tracking restoration failed during removal: %d\n",
				 r);
			break;
		}
		mutex_unlock(&donation->lock);

		/*
		 * Continuing while Linux owns pages, or after an incomplete CPA
		 * change, would dismantle TTM around live System RAM.  Retry until
		 * transient pins and mapping failures clear.
		 */
		dev_crit_ratelimited(adev->dev,
				     "waiting to return donated System RAM during device removal: %d\n",
				     r);
		msleep(1000);
	}

	donation->supported = false;
	unregister_memory_notifier(&donation->memory_nb);
	mutex_destroy(&donation->lock);
	kfree(donation->guards);
	donation->guards = NULL;
}

#else

int amdgpu_mem_donation_init(struct amdgpu_device *adev)
{
	return 0;
}

void amdgpu_mem_donation_fini(struct amdgpu_device *adev)
{
}

int amdgpu_mem_donation_set_size(struct amdgpu_device *adev, u64 size)
{
	return -EOPNOTSUPP;
}

u64 amdgpu_mem_donation_get_size(struct amdgpu_device *adev)
{
	return 0;
}

u64 amdgpu_mem_donation_get_quarantined_size(struct amdgpu_device *adev)
{
	return 0;
}

int
amdgpu_mem_donation_begin(struct amdgpu_device *adev,
			  enum amdgpu_mem_donation_lifecycle lifecycle,
			  bool rollback)
{
	return 0;
}

int amdgpu_mem_donation_shutdown_begin(struct amdgpu_device *adev)
{
	return 0;
}

bool amdgpu_mem_donation_shutdown_safe(struct amdgpu_device *adev)
{
	return true;
}

void
amdgpu_mem_donation_end(struct amdgpu_device *adev,
			enum amdgpu_mem_donation_lifecycle lifecycle,
			bool restore)
{
}

#endif
