// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2016-2019 Intel Corporation. All rights reserved. */
#include <linux/memremap.h>
#include <linux/pagemap.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/memory-tiers.h>
#include <linux/memory_hotplug.h>
#include <linux/cram.h>
#include <linux/string_helpers.h>
#include "dax-private.h"
#include "bus.h"

/*
 * Default abstract distance assigned to the NUMA node onlined
 * by DAX/kmem if the low level platform driver didn't initialize
 * one for this NUMA node.
 */
#define MEMTIER_DEFAULT_DAX_ADISTANCE	(MEMTIER_ADISTANCE_DRAM * 5)

/* Memory resource name used for add_memory_driver_managed(). */
static const char *kmem_name;
/* Set if any memory will remain added when the driver will be unloaded. */
static bool any_hotremove_failed;

static int dax_kmem_range(struct dev_dax *dev_dax, int i, struct range *r)
{
	struct dev_dax_range *dax_range = &dev_dax->ranges[i];
	struct range *range = &dax_range->range;

	*r = memory_block_aligned_range(range);
	if (r->start >= r->end) {
		r->start = range->start;
		r->end = range->end;
		return -ENOSPC;
	}
	return 0;
}

/*
 * dax_kmem_cram_ranges() - [TEST] build the block-aligned ranges donated to CRAM
 *
 * Builds the range array this dev_dax donates to CRAM.  The result is
 * deterministic, so cram_register() and cram_unregister() produce the identical
 * set.  The caller frees it.  Returns NULL on alloc failure or when no range is
 * usable.
 */
static struct range *dax_kmem_cram_ranges(struct dev_dax *dev_dax, unsigned int *np)
{
	struct range *ranges;
	unsigned int n = 0;
	int i;

	ranges = kmalloc_array(dev_dax->nr_range, sizeof(*ranges), GFP_KERNEL);
	if (!ranges)
		return NULL;
	for (i = 0; i < dev_dax->nr_range; i++)
		if (!dax_kmem_range(dev_dax, i, &ranges[n]))
			n++;
	if (!n) {
		kfree(ranges);
		return NULL;
	}
	*np = n;
	return ranges;
}

struct dax_kmem_data {
	const char *res_name;
	int mgid;
	int state;
	struct mutex lock; /* protects hotplug state transitions and config */
	bool cram; /* when set, CRAM manages the node as a read-only tier */
	u32 cram_zratio; /* [TEST] hw compression ratio handed to cram_register (per-mille) */
	int cram_adistance; /* [TEST] abstract distance, 0 = CRAM's default */
	unsigned int cram_trim_mode; /* [TEST] 0 none(zero) 1 succeed 2 EAGAIN 3 EBUSY */
	unsigned long cram_trim_pages; /* [TEST] pages successfully trimmed via the cb */
	struct resource *res[];
};

static DEFINE_MUTEX(kmem_memory_type_lock);
static LIST_HEAD(kmem_memory_types);

static struct memory_dev_type *kmem_find_alloc_memory_type(int adist)
{
	guard(mutex)(&kmem_memory_type_lock);
	return mt_find_alloc_memory_type(adist, &kmem_memory_types);
}

static void kmem_put_memory_types(void)
{
	guard(mutex)(&kmem_memory_type_lock);
	mt_put_memory_types(&kmem_memory_types);
}

/* True for the online states a kmem dax device can hold. */
static bool dax_kmem_state_is_online(int state)
{
	return state == MMOP_ONLINE ||
	       state == MMOP_ONLINE_KERNEL ||
	       state == MMOP_ONLINE_MOVABLE;
}

/**
 * dax_kmem_do_hotplug - hotplug memory for dax kmem device
 * @dev_dax: the dev_dax instance
 * @data: the dax_kmem_data structure with resource tracking
 * @online_type: the online policy to use for the memory blocks
 *
 * Hotplugs all ranges in the dev_dax region as system memory with the
 * provided online policy (offline, online, online_movable, online_kernel).
 *
 * Returns the number of successfully mapped ranges, or negative error.
 */
static int dax_kmem_do_hotplug(struct dev_dax *dev_dax,
			       struct dax_kmem_data *data,
			       int online_type)
{
	struct device *dev = &dev_dax->dev;
	int i, rc, added = 0;
	mhp_t mhp_flags;

	if (dax_kmem_state_is_online(data->state))
		return -EINVAL;

	if (online_type < MMOP_OFFLINE || online_type > MMOP_ONLINE_MOVABLE)
		return -EINVAL;

	for (i = 0; i < dev_dax->nr_range; i++) {
		struct range range;

		rc = dax_kmem_range(dev_dax, i, &range);
		if (rc)
			continue;

		/*
		 * init_resources() is best-effort: if a reservation conflict
		 * occurs it keeps the range but leaves res[i]=NULL. For hotplug
		 * on probe systems, this means kmem will partially online.
		 *
		 * We have to keep this behavior not to break those systems.
		 * For those systems - atomicity only applies to valid ranges.
		 */
		if (!data->res[i])
			continue;

		mhp_flags = MHP_NID_IS_MGID;
		if (dev_dax->memmap_on_memory)
			mhp_flags |= MHP_MEMMAP_ON_MEMORY;

		/*
		 * Ensure that future kexec'd kernels will not treat
		 * this as RAM automatically.
		 */
		rc = __add_memory_driver_managed(data->mgid, range.start,
				range_len(&range), kmem_name, mhp_flags,
				online_type, dev_dax->mm_features);

		if (rc) {
			dev_warn(dev, "mapping%d: %#llx-%#llx memory add failed\n",
				 i, range.start, range.end);
			/*
			 * Release the reservation for the range that failed to
			 * add so a later hotremove does not try to remove memory
			 * that was never added.
			 */
			if (data->res[i]) {
				remove_resource(data->res[i]);
				kfree(data->res[i]);
				data->res[i] = NULL;
			}
			if (added)
				continue;
			return rc;
		}
		added++;
	}

	return added;
}

/**
 * dax_kmem_init_resources - create memory regions for dax kmem
 * @dev_dax: the dev_dax instance
 * @data: the dax_kmem_data structure with resource tracking
 *
 * Initializes all the resources for the DAX
 *
 * Returns the number of successfully mapped ranges, or negative error.
 */
static int dax_kmem_init_resources(struct dev_dax *dev_dax,
				   struct dax_kmem_data *data)
{
	struct device *dev = &dev_dax->dev;
	int i, rc, mapped = 0;

	for (i = 0; i < dev_dax->nr_range; i++) {
		struct resource *res;
		struct range range;

		rc = dax_kmem_range(dev_dax, i, &range);
		if (rc)
			continue;

		/* Skip ranges already added */
		if (data->res[i])
			continue;

		/* Region is permanently reserved if hotremove fails. */
		res = request_mem_region(range.start, range_len(&range),
					 data->res_name);
		if (!res) {
			dev_warn(dev, "mapping%d: %#llx-%#llx could not reserve region\n",
				 i, range.start, range.end);
			/*
			 * Once some memory has been onlined we can't
			 * assume that it can be un-onlined safely.
			 */
			if (mapped)
				continue;
			return -EBUSY;
		}
		data->res[i] = res;
		/*
		 * Set flags appropriate for System RAM.  Leave ..._BUSY clear
		 * so that add_memory() can add a child resource.  Do not
		 * inherit flags from the parent since it may set new flags
		 * unknown to us that will break add_memory() later.
		 */
		res->flags = IORESOURCE_SYSTEM_RAM;
		mapped++;
	}
	return mapped;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/**
 * dax_kmem_do_hotremove - hot-remove memory for dax kmem device
 * @dev_dax: the dev_dax instance
 * @data: the dax_kmem_data structure with resource tracking
 *
 * Offlines and removes every currently-added range in the dev_dax region
 * atomically: either all ranges are offlined and removed, or none are and
 * the device is returned to its prior state.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
static int dax_kmem_do_hotremove(struct dev_dax *dev_dax,
				 struct dax_kmem_data *data)
{
	struct device *dev = &dev_dax->dev;
	struct range *ranges;
	int i, nr_ranges = 0, rc;

	ranges = kmalloc_objs(*ranges, dev_dax->nr_range);
	if (!ranges)
		return -ENOMEM;

	/* Collect the ranges that were actually added during probe. */
	for (i = 0; i < dev_dax->nr_range; i++) {
		struct range range;

		if (!data->res[i])
			continue;
		if (dax_kmem_range(dev_dax, i, &range))
			continue;
		ranges[nr_ranges++] = range;
	}

	/* Nothing added means nothing to remove. */
	if (!nr_ranges) {
		kfree(ranges);
		return 0;
	}

	rc = offline_and_remove_memory_ranges(ranges, nr_ranges);
	kfree(ranges);
	if (rc) {
		/* Recoverable: the ranges rolled back, nothing is leaked yet. */
		dev_err(dev, "hotremove failed, device left online: %d\n", rc);
		return rc;
	}

	/* All ranges removed; release the reserved resources. */
	for (i = 0; i < dev_dax->nr_range; i++) {
		if (!data->res[i])
			continue;
		remove_resource(data->res[i]);
		kfree(data->res[i]);
		data->res[i] = NULL;
	}

	return 0;
}
#else
static int dax_kmem_do_hotremove(struct dev_dax *dev_dax,
				 struct dax_kmem_data *data)
{
	return -EBUSY;
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

/**
 * dax_kmem_cleanup_resources - remove the dax memory resources
 * @dev_dax: the dev_dax instance
 * @data: the dax_kmem_data structure with resource tracking
 *
 * Removes all resources in the dev_dax region.
 */
static void dax_kmem_cleanup_resources(struct dev_dax *dev_dax,
				       struct dax_kmem_data *data)
{
	int i;

	/*
	 * If the device unbind occurs before memory is hotremoved, we can never
	 * remove the memory (requires reboot).  Attempting an offline operation
	 * here may cause deadlock and a failure to finish the unbind.
	 *
	 * Note: This leaks the resources.
	 */
	if (WARN(((data->state != DAX_KMEM_UNPLUGGED) &&
		  (data->state != MMOP_OFFLINE)),
		 "Hotplug memory regions stuck online until reboot"))
		return;

	for (i = 0; i < dev_dax->nr_range; i++) {
		if (!data->res[i])
			continue;
		remove_resource(data->res[i]);
		kfree(data->res[i]);
		data->res[i] = NULL;
	}
}

static int dax_kmem_parse_state(const char *buf)
{
	int online_type;

	/* "unplugged" is kmem-specific - the rest map to MMOP_ */
	if (sysfs_streq(buf, "unplugged"))
		return DAX_KMEM_UNPLUGGED;

	online_type = mhp_online_type_from_str(buf);
	/* Disallow "offline": it's not useful and creates race conditions */
	if (online_type == MMOP_OFFLINE)
		return -EINVAL;
	return online_type;
}

/*
 * dax_kmem_cram_trim() - [TEST] CRAM trim callback
 *
 * A real driver tells its hardware to drop the compression backing for the
 * freed pages.  This stand-in only accounts them.  It honors a debug mode so
 * the test can exercise CRAM's retry and zero-fallback contract:
 *   1 succeed (default), 2 return -EAGAIN (CRAM retries), 3 return -EBUSY
 *   (CRAM zeroes).  Mode 0 registers no callback at all (CRAM always zeroes).
 */
static int dax_kmem_cram_trim(void *driver_data, const unsigned long *pfns,
			      int *result, unsigned int nr_pages)
{
	struct dax_kmem_data *data = driver_data;
	unsigned int i;

	switch (READ_ONCE(data->cram_trim_mode)) {
	case 2:
		for (i = 0; i < nr_pages; i++)
			result[i] = -1;
		return -EAGAIN;
	case 3:
		for (i = 0; i < nr_pages; i++)
			result[i] = -1;
		return -EBUSY;
	default:
		for (i = 0; i < nr_pages; i++)
			result[i] = 0;
		data->cram_trim_pages += nr_pages;	/* serialized by balloon_mutex */
		return 0;
	}
}

static ssize_t state_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	const char *state_str;

	if (data->state == DAX_KMEM_UNPLUGGED)
		state_str = "unplugged";
	else
		state_str = mhp_online_type_to_str(data->state);

	return sysfs_emit(buf, "%s\n", state_str ?: "unknown");
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct dev_dax *dev_dax = to_dev_dax(dev);
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	int online_type;
	int rc;

	online_type = dax_kmem_parse_state(buf);
	if (online_type < DAX_KMEM_UNPLUGGED)
		return online_type;

	guard(mutex)(&data->lock);

	/* Already in requested state */
	if (data->state == online_type)
		return len;

	if (online_type == DAX_KMEM_UNPLUGGED) {
		if (data->cram) {
			struct range *ranges;
			unsigned int n;

			/* CRAM owns the node: evict + remove all donated ranges. */
			ranges = dax_kmem_cram_ranges(dev_dax, &n);
			if (!ranges)
				return -ENOMEM;
			rc = cram_unregister(dev_dax->target_node, ranges, n);
			kfree(ranges);
			if (rc)
				return rc;
			data->state = DAX_KMEM_UNPLUGGED;
			return len;
		}
		rc = dax_kmem_do_hotremove(dev_dax, data);
		if (rc)
			return rc;
		data->state = DAX_KMEM_UNPLUGGED;
		return len;
	}

	/* Onlining is only allowed from the unplugged state. */
	if (data->state != DAX_KMEM_UNPLUGGED)
		return -EBUSY;

	if (data->cram) {
		struct cram_ops ops = { .owner = THIS_MODULE,
					.trim = data->cram_trim_mode ?
						dax_kmem_cram_trim : NULL };
		struct range *ranges;
		unsigned int n;

		/*
		 * Hand all donated ranges to CRAM.  CRAM does the private-node hotplug
		 * (always movable) and cap registration itself, so ignore the requested
		 * online_type.  kmem reserves no resources here.  @data is the ops
		 * callback cookie.
		 */
		ranges = dax_kmem_cram_ranges(dev_dax, &n);
		if (!ranges)
			return -ENOMEM;
		rc = cram_register(dev_dax->target_node, ranges, n,
				   data->cram_zratio, data->cram_adistance,
				   ops, data);
		kfree(ranges);
		if (rc)
			return rc;
		data->state = MMOP_ONLINE_MOVABLE;
		return len;
	}

	/* Re-acquire resources if previously unplugged, otherwise no-op */
	rc = dax_kmem_init_resources(dev_dax, data);
	if (rc < 0)
		return rc;

	rc = dax_kmem_do_hotplug(dev_dax, data, online_type);
	if (rc < 0) {
		/* Total failure, drop the reservations we took. */
		dax_kmem_cleanup_resources(dev_dax, data);
		return rc;
	}

	data->state = online_type;
	return len;
}

/*
 * [TEST] 'cram' control: hand this device's range to CRAM, which manages it as
 * a read-only tier.  CRAM owns the hotplug for such a node and registers the
 * feature mask itself, so this device's own mm_features do not apply.
 * Onlining routes through cram_register(); see state_store().
 */
static ssize_t cram_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", data->cram);
}

static ssize_t cram_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	bool enable;
	ssize_t rc;

	rc = kstrtobool(buf, &enable);
	if (rc)
		return rc;

	guard(mutex)(&data->lock);

	if (data->state != DAX_KMEM_UNPLUGGED)
		return -EBUSY;

	/*
	 * CRAM owns this node's hotplug and registers its own feature mask, so
	 * dev_dax->mm_features never reaches the node and is not consulted
	 * here.  Just record the mode; cram_register() decides the rest.
	 */
	data->cram = enable;
	if (enable && !data->cram_zratio)
		data->cram_zratio = 1000;	/* default 1:1 until set via cram_zratio */
	if (enable && !data->cram_trim_mode)
		data->cram_trim_mode = 1;	/* default: trim callback succeeds */
	return len;
}
static DEVICE_ATTR_RW(cram);

/*
 * [TEST] 'cram_zratio': hardware compression ratio handed to cram_register(),
 * per-mille N:1 (3:1 => 3000, 1:1 => 1000).  Configure while unplugged.
 */
static ssize_t cram_zratio_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->cram_zratio);
}

static ssize_t cram_zratio_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t len)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	u32 zratio;
	ssize_t rc;

	rc = kstrtou32(buf, 0, &zratio);
	if (rc)
		return rc;
	if (zratio < 1000)		/* < 1:1 is nonsensical */
		return -EINVAL;

	guard(mutex)(&data->lock);
	if (data->state != DAX_KMEM_UNPLUGGED)
		return -EBUSY;
	data->cram_zratio = zratio;
	return len;
}
static DEVICE_ATTR_RW(cram_zratio);

/*
 * [TEST] 'cram_adistance': abstract distance handed to cram_register(), which
 * decides the tier the node lands in and therefore whether reclaim will demote
 * to it.  0 leaves it to CRAM's default.  Configure while unplugged.
 */
static ssize_t cram_adistance_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", data->cram_adistance);
}

static ssize_t cram_adistance_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	int adist;
	ssize_t rc;

	rc = kstrtoint(buf, 0, &adist);
	if (rc)
		return rc;
	if (adist < 0)
		return -EINVAL;

	guard(mutex)(&data->lock);
	if (data->state != DAX_KMEM_UNPLUGGED)
		return -EBUSY;
	data->cram_adistance = adist;
	return len;
}
static DEVICE_ATTR_RW(cram_adistance);

/*
 * [TEST] 'cram_compression_ratio': report the achieved compression ratio (per-mille
 * N:1) to CRAM, which resizes the balloon.  Write "R" or "R block".  "R block"
 * blocks new allocations for the duration of the adjustment; a danger mode.
 */
static ssize_t cram_compression_ratio_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	struct dev_dax *dev_dax = to_dev_dax(dev);
	char tok[16] = "";
	u32 ratio;
	int rc;

	if (sscanf(buf, "%u %15s", &ratio, tok) < 1)
		return -EINVAL;

	guard(mutex)(&data->lock);
	if (!data->cram)
		return -EINVAL;
	rc = cram_set_compression_ratio(dev_dax->target_node, ratio,
					!strcmp(tok, "block"));
	return rc ? rc : len;
}
static DEVICE_ATTR_WO(cram_compression_ratio);

/*
 * [TEST] 'cram_allow_allocation': sticky enable/disable of demotions onto the
 * node.  This is the emergency off switch, distinct from cram_compression_ratio's
 * transient "block".  Write 0 to revoke, 1 to re-permit.
 */
static ssize_t cram_allow_allocation_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t len)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	struct dev_dax *dev_dax = to_dev_dax(dev);
	bool allow;
	int rc;

	rc = kstrtobool(buf, &allow);
	if (rc)
		return rc;

	guard(mutex)(&data->lock);
	if (!data->cram)
		return -EINVAL;
	rc = cram_allow_allocation(dev_dax->target_node, allow);
	return rc ? rc : len;
}
static DEVICE_ATTR_WO(cram_allow_allocation);

/*
 * [TEST] 'cram_hot_pages': stand in for the device's hotness reporter.  Write a
 * whitespace-separated list of pfns the "device" deems hot.  They are forwarded
 * to cram_report_hot_pages() for proactive promotion.  A real driver reports the
 * pfns its hardware observed being accessed; here userland supplies them from
 * pagemap.
 */
#define CRAM_HOT_MAX 1024
static ssize_t cram_hot_pages_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	struct dev_dax *dev_dax = to_dev_dax(dev);
	unsigned long *pfns;
	unsigned int n = 0;
	const char *p = buf;
	int rc;

	pfns = kmalloc_array(CRAM_HOT_MAX, sizeof(*pfns), GFP_KERNEL);
	if (!pfns)
		return -ENOMEM;

	while (n < CRAM_HOT_MAX) {
		unsigned long v;
		int consumed;

		if (sscanf(p, "%lu%n", &v, &consumed) != 1)
			break;
		pfns[n++] = v;
		p += consumed;
	}

	scoped_guard(mutex, &data->lock) {
		if (!data->cram) {
			rc = -EINVAL;
			goto out;
		}
		rc = cram_report_hot_pages(dev_dax->target_node, pfns, n);
	}
out:
	kfree(pfns);
	return rc ? rc : len;
}
static DEVICE_ATTR_WO(cram_hot_pages);

/*
 * [TEST] 'cram_trim': trim-callback behavior, configure while unplugged.
 *   0 no callback (CRAM zeroes freed pages)   1 succeed (default)
 *   2 return -EAGAIN (CRAM retries)           3 return -EBUSY (CRAM zeroes)
 */
static ssize_t cram_trim_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->cram_trim_mode);
}

static ssize_t cram_trim_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	unsigned int mode;

	if (kstrtouint(buf, 0, &mode) || mode > 3)
		return -EINVAL;

	guard(mutex)(&data->lock);
	if (data->state != DAX_KMEM_UNPLUGGED)
		return -EBUSY;		/* ops.trim is fixed at register time */
	data->cram_trim_mode = mode;
	return len;
}
static DEVICE_ATTR_RW(cram_trim);

/* [TEST] 'cram_trim_count': pages the trim callback has successfully trimmed. */
static ssize_t cram_trim_count_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(data->cram_trim_pages));
}
static DEVICE_ATTR_RO(cram_trim_count);
static int dev_dax_kmem_probe(struct dev_dax *dev_dax)
{
	struct device *dev = &dev_dax->dev;
	unsigned long total_len = 0, orig_len = 0;
	struct dax_kmem_data *data;
	struct memory_dev_type *mtype;
	int i, rc;
	int numa_node;
	int adist = MEMTIER_DEFAULT_DAX_ADISTANCE;
	int online_type = mhp_get_default_online_type();

	/*
	 * Ensure good NUMA information for the persistent memory.
	 * Without this check, there is a risk that slow memory
	 * could be mixed in a node with faster memory, causing
	 * unavoidable performance issues.
	 */
	numa_node = dev_dax->target_node;
	if (numa_node < 0) {
		dev_warn(dev, "rejecting DAX region with invalid node: %d\n",
				numa_node);
		return -EINVAL;
	}

	mt_calc_adistance(numa_node, &adist);
	mtype = kmem_find_alloc_memory_type(adist);
	if (IS_ERR(mtype))
		return PTR_ERR(mtype);

	for (i = 0; i < dev_dax->nr_range; i++) {
		struct range range;

		orig_len += range_len(&dev_dax->ranges[i].range);
		rc = dax_kmem_range(dev_dax, i, &range);
		if (rc) {
			dev_info(dev, "mapping%d: %#llx-%#llx too small after alignment\n",
					i, range.start, range.end);
			continue;
		}
		total_len += range_len(&range);
	}

	if (!total_len) {
		dev_warn(dev, "rejecting DAX region without any memory after alignment\n");
		return -EINVAL;
	} else if (total_len != orig_len) {
		char buf[16];

		string_get_size(orig_len - total_len, 1, STRING_UNITS_2,
				buf, sizeof(buf));
		dev_warn(dev, "DAX region truncated by %s due to alignment\n", buf);
	}

	init_node_memory_type(numa_node, mtype);

	rc = -ENOMEM;
	data = kzalloc_flex(*data, res, dev_dax->nr_range);
	if (!data)
		goto err_dax_kmem_data;

	data->res_name = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!data->res_name)
		goto err_res_name;

	rc = memory_group_register_static(numa_node, PFN_UP(total_len));
	if (rc < 0)
		goto err_reg_mgid;
	data->mgid = rc;
	data->state = DAX_KMEM_UNPLUGGED;
	mutex_init(&data->lock);

	dev_set_drvdata(dev, data);

	rc = dax_kmem_init_resources(dev_dax, data);
	if (rc < 0)
		goto err_resources;

	rc = dax_kmem_do_hotplug(dev_dax, data, online_type);
	if (rc < 0)
		goto err_hotplug;
	data->state = online_type;

	return 0;

err_hotplug:
	dax_kmem_cleanup_resources(dev_dax, data);
err_resources:
	dev_set_drvdata(dev, NULL);
	memory_group_unregister(data->mgid);
err_reg_mgid:
	kfree(data->res_name);
err_res_name:
	kfree(data);
err_dax_kmem_data:
	clear_node_memory_type(numa_node, mtype);
	return rc;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/*
 * Remove the device's added ranges with remove_memory().
 * Unlike the sysfs unplug path it never offlines and fails if the blocks are
 * online (-EBUSY), so it is safe from unbind. Failures leak until reboot.
 *
 * Returns 0 only if every added range was removed.
 */
static int dax_kmem_remove_ranges(struct dev_dax *dev_dax,
				  struct dax_kmem_data *data)
{
	struct device *dev = &dev_dax->dev;
	int i, rc = 0;

	for (i = 0; i < dev_dax->nr_range; i++) {
		struct range range;

		if (!data->res[i] || dax_kmem_range(dev_dax, i, &range))
			continue;
		if (remove_memory(range.start, range_len(&range))) {
			dev_warn(dev, "mapping%d: %#llx-%#llx stuck online until reboot\n",
				 i, range.start, range.end);
			rc = -EBUSY;
			continue;
		}
		remove_resource(data->res[i]);
		kfree(data->res[i]);
		data->res[i] = NULL;
	}
	return rc;
}

static void dev_dax_kmem_remove(struct dev_dax *dev_dax)
{
	int node = dev_dax->target_node;
	struct device *dev = &dev_dax->dev;
	struct dax_kmem_data *data = dev_get_drvdata(dev);

	/*
	 * Remove every range that is still added.  dax_kmem_remove_ranges()
	 * uses remove_memory(), which never offlines: an online block fails
	 * with -EBUSY rather than deadlocking an uninterruptible unbind.
	 *
	 * data->state only tracks daxX.Y/state writes, so it can be stale if
	 * blocks were toggled via memoryX/state. Do not trust it here and
	 * attempt simply remove_memory() - which reports the true state of
	 * each range anyway. Anything left online is leaked until reboot.
	 */
	if (dax_kmem_remove_ranges(dev_dax, data)) {
		dev_err(dev, "Hotplug regions stuck online until reboot\n");
		any_hotremove_failed = true;
		return;
	}

	memory_group_unregister(data->mgid);
	kfree(data->res_name);
	kfree(data);
	dev_set_drvdata(dev, NULL);
	/*
	 * Clear the memtype association on successful unplug.
	 * If not, we have memory blocks left which can be
	 * offlined/onlined later. We need to keep memory_dev_type
	 * for that. This implies this reference will be around
	 * till next reboot.
	 */
	clear_node_memory_type(node, NULL);
}
#else
static void dev_dax_kmem_remove(struct dev_dax *dev_dax)
{
	/*
	 * Without hotremove purposely leak the request_mem_region() for the
	 * device-dax range and return '0' to ->remove() attempts. The removal
	 * of the device from the driver always succeeds, but the region is
	 * permanently pinned as reserved by the unreleased
	 * request_mem_region().
	 */
	any_hotremove_failed = true;
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

static DEVICE_ATTR_RW(state);

static struct attribute *dev_dax_kmem_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_cram.attr,
	&dev_attr_cram_zratio.attr,
	&dev_attr_cram_adistance.attr,
	&dev_attr_cram_compression_ratio.attr,
	&dev_attr_cram_allow_allocation.attr,
	&dev_attr_cram_hot_pages.attr,
	&dev_attr_cram_trim.attr,
	&dev_attr_cram_trim_count.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dev_dax_kmem);

static struct dax_device_driver device_dax_kmem_driver = {
	.probe = dev_dax_kmem_probe,
	.remove = dev_dax_kmem_remove,
	.type = DAXDRV_KMEM_TYPE,
	.drv = {
		.dev_groups = dev_dax_kmem_groups,
	},
};

static int __init dax_kmem_init(void)
{
	int rc;

	/* Resource name is permanently allocated if any hotremove fails. */
	kmem_name = kstrdup_const("System RAM (kmem)", GFP_KERNEL);
	if (!kmem_name)
		return -ENOMEM;

	rc = dax_driver_register(&device_dax_kmem_driver);
	if (rc)
		goto error_dax_driver;

	return rc;

error_dax_driver:
	kmem_put_memory_types();
	kfree_const(kmem_name);
	return rc;
}

static void __exit dax_kmem_exit(void)
{
	dax_driver_unregister(&device_dax_kmem_driver);
	if (!any_hotremove_failed)
		kfree_const(kmem_name);
	kmem_put_memory_types();
}

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("KMEM DAX: map dax-devices as System-RAM");
MODULE_LICENSE("GPL v2");
module_init(dax_kmem_init);
module_exit(dax_kmem_exit);
MODULE_ALIAS_DAX_DEVICE(0);
