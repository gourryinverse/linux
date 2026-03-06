/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NODE_DEVICE_H
#define _LINUX_NODE_DEVICE_H

#include <linux/completion.h>
#include <linux/memremap.h>
#include <linux/migrate_mode.h>
#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/refcount.h>

struct page;
struct vm_area_struct;
struct vm_fault;

/**
 * struct node_device - Per-node container for managed device nodes
 *
 * Tracks multiple dev_pagemap instances per node and holds per-node state
 * for managed device node pages.  Stored in pgdat->node_dev.
 *
 * Each hotplugged memory range has a dev_pagemap registered via
 * node_device_add_pgmap().  The pgmaps serve as metadata containers
 * for ops callbacks and capability flags.  Memory is hotplugged into
 * the buddy allocator via __add_memory_driver_managed().
 *
 * The driver allocates and owns this structure.  It must remain valid
 * until node_device_unregister() returns with refcount dropped to 0.
 *
 * Per-pgmap callbacks (folio_split, handle_fault, folio_migrate, etc.)
 * live on struct dev_pagemap_ops.  Since managed folios are buddy-managed
 * and don't have per-page pgmap pointers, callbacks are dispatched via
 * node_device_find_pgmap(nid, pfn) to look up the correct pgmap.
 *
 * Per-node callbacks (migrate_to, reclaim_policy) are added to this
 * structure by subsequent commits as each consumer is wired up.
 *
 * @owner: Opaque driver identifier
 * @pgmaps: Dynamic array of dev_pagemap pointers (one per hotplug range)
 * @nr_pgmaps: Number of pgmaps currently registered
 * @flags: Union of all PGMAP_OPS_* flags from registered pgmaps
 * @migrate_to: Migrate folios TO this node.  Returns 0 on full success,
 *      >0 = number of folios that failed, <0 = error.
 *      Matches migrate_pages() semantics.
 * @refcount: Reference count (1 = registered; 0 = fully released)
 * @released: Signaled when refcount drops to 0; unregister waits on this
 */
struct node_device {
	void *owner;
	struct dev_pagemap **pgmaps;
	int nr_pgmaps;
	unsigned long flags;
	int (*migrate_to)(struct list_head *folios, int nid,
			  enum migrate_mode mode,
			  enum migrate_reason reason,
			  unsigned int *nr_succeeded);
	refcount_t refcount;
	struct completion released;
};

#ifdef CONFIG_NUMA

#include <linux/mmzone.h>

/**
 * node_device_flags - Get the PGMAP_OPS_* flags for a node
 * @nid: Node identifier
 *
 * Returns the union of all PGMAP_OPS_* flags for the node's pgmaps,
 * or 0 if the node has no node_device.
 */
static inline unsigned long node_device_flags(int nid)
{
	struct node_device *nd;
	unsigned long flags;

	rcu_read_lock();
	nd = rcu_dereference(NODE_DATA(nid)->node_dev);
	flags = nd ? nd->flags : 0;
	rcu_read_unlock();

	return flags;
}

/**
 * node_device_has_flag - Check if a node has a specific PGMAP_OPS_* flag
 * @nid: Node identifier
 * @flag: PGMAP_OPS_* flag to check
 */
static inline bool node_device_has_flag(int nid, unsigned long flag)
{
	return node_device_flags(nid) & flag;
}

/**
 * zone_device_has_flag - Check if a zone's node has a specific PGMAP_OPS_* flag
 * @z: The zone to check
 * @flag: PGMAP_OPS_* flag to check
 */
static inline bool zone_device_has_flag(struct zone *z, unsigned long flag)
{
	return node_device_flags(zone_to_nid(z)) & flag;
}

/**
 * node_device_find_pgmap - Find the pgmap covering a given PFN on a node
 * @nid: Node identifier
 * @pfn: Page frame number to look up
 *
 * Searches the node_device's pgmap array for a pgmap whose physical address
 * range contains @pfn.  Returns the matching pgmap, or NULL if none found
 * or the node has no node_device.
 *
 * Must be called with RCU read lock held (or from a context where the
 * node_device is guaranteed to remain valid).
 */
static inline struct dev_pagemap *node_device_find_pgmap(int nid,
							 unsigned long pfn)
{
	struct node_device *nd;
	int i;

	nd = rcu_dereference(NODE_DATA(nid)->node_dev);
	if (!nd)
		return NULL;

	for (i = 0; i < nd->nr_pgmaps; i++) {
		struct dev_pagemap *pgmap = nd->pgmaps[i];
		int j;

		for (j = 0; j < pgmap->nr_range; j++) {
			unsigned long start = PHYS_PFN(pgmap->ranges[j].start);
			unsigned long end = PHYS_PFN(pgmap->ranges[j].end);

			if (pfn >= start && pfn <= end)
				return pgmap;
		}
	}

	return NULL;
}

/**
 * node_device_migrate_to - Migrate folios to a managed device node
 * @folios: list of folios to migrate
 * @nid: target node
 * @mode: migration mode (MIGRATE_ASYNC, MIGRATE_SYNC, etc.)
 * @reason: migration reason (MR_DEMOTION, MR_SYSCALL, etc.)
 * @nr_succeeded: optional output for number of successfully migrated folios
 *
 * If @nid has a node_device with a migrate_to callback, invokes it.
 * Returns 0 on full success, >0 = failure count, <0 = error.
 * Returns -ENODEV if the node has no node_device or no migrate_to callback.
 */
static inline int node_device_migrate_to(struct list_head *folios, int nid,
					 enum migrate_mode mode,
					 enum migrate_reason reason,
					 unsigned int *nr_succeeded)
{
	int (*fn)(struct list_head *, int, enum migrate_mode,
		  enum migrate_reason, unsigned int *);
	struct node_device *nd;
	int ret;

	rcu_read_lock();
	nd = rcu_dereference(NODE_DATA(nid)->node_dev);
	if (!nd || !nd->migrate_to ||
	    !refcount_inc_not_zero(&nd->refcount)) {
		rcu_read_unlock();
		return -ENODEV;
	}
	fn = nd->migrate_to;
	rcu_read_unlock();

	ret = fn(folios, nid, mode, reason, nr_succeeded);

	if (refcount_dec_and_test(&nd->refcount))
		complete(&nd->released);

	return ret;
}

#else /* !CONFIG_NUMA */

static inline unsigned long node_device_flags(int nid)
{
	return 0;
}

static inline bool node_device_has_flag(int nid, unsigned long flag)
{
	return false;
}

static inline bool zone_device_has_flag(struct zone *z, unsigned long flag)
{
	return false;
}

static inline struct dev_pagemap *node_device_find_pgmap(int nid,
							 unsigned long pfn)
{
	return NULL;
}

static inline int node_device_migrate_to(struct list_head *folios, int nid,
					 enum migrate_mode mode,
					 enum migrate_reason reason,
					 unsigned int *nr_succeeded)
{
	return -ENODEV;
}

#endif /* CONFIG_NUMA */

#if defined(CONFIG_NUMA) && defined(CONFIG_MEMORY_HOTPLUG)

int node_device_register(int nid, struct node_device *nd);
int node_device_unregister(int nid);
int node_device_add_pgmap(int nid, struct dev_pagemap *pgmap);
int node_device_remove_pgmap(int nid, struct dev_pagemap *pgmap);

#else /* !CONFIG_NUMA || !CONFIG_MEMORY_HOTPLUG */

static inline int node_device_register(int nid, struct node_device *nd)
{
	return -ENODEV;
}

static inline int node_device_unregister(int nid)
{
	return 0;
}

static inline int node_device_add_pgmap(int nid, struct dev_pagemap *pgmap)
{
	return -ENODEV;
}

static inline int node_device_remove_pgmap(int nid, struct dev_pagemap *pgmap)
{
	return -ENODEV;
}

#endif /* CONFIG_NUMA && CONFIG_MEMORY_HOTPLUG */

#endif /* _LINUX_NODE_DEVICE_H */
