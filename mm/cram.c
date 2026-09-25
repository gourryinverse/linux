// SPDX-License-Identifier: GPL-2.0
/*
 * Compressed RAM / private node memory management
 *
 * Manages anonymous folios placed on compressed private memory.  Reclaim
 * diverts eligible folios through a CRAM-specific migration path rather than
 * adding the node to the generic memory-tier topology.  Folios remain present
 * and readable; a write fault copies them back to common memory.
 *
 * A driver donates physical regions and registers as a node's CRAM owner with
 * cram_register().
 *
 * CRAM does not depend on swap/vswap.  Entry is by migration and resident CRAM
 * pages are ordinary anon LRU folios on the private node.
 */

#include <linux/cram.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/memory_hotplug.h>
#include <linux/node.h>
#include <linux/list.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nodemask.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

/*
 * RECLAIM lets pressure on the CRAM node swap resident anonymous folios out.
 * WR_FENCE is set, so userspace writes copy the folio to common memory.
 * Incoming placement is owned by the explicit CRAM migration
 * path below, not by generic tiering.
 */
struct cram_node {
	int			nid;
	struct range		*ranges;	/* donated regions */
	unsigned int		nr_ranges;
};

static struct cram_node __rcu *cram_nodes[MAX_NUMNODES];
static DEFINE_MUTEX(cram_mutex);

/*
 * The set of registered CRAM nodes, mirroring cram_nodes[] as a bitmap.  Bits
 * are flipped under cram_mutex at register/unregister and read locklessly, like
 * node_states.  CRAM's own walks iterate it directly (for_each_node_mask)
 * instead of filtering the wider node-private set.  The array owns each node's
 * control state; the mask provides the fast membership test.
 */
static nodemask_t cram_node_mask __read_mostly;

static inline bool cram_valid_nid(int nid)
{
	return nid >= 0 && nid < MAX_NUMNODES;
}

static bool __cram_node_usable(const struct cram_node *cn)
{
	struct zone *zone;

	if (!cn)
		return false;
	zone = &NODE_DATA(cn->nid)->node_zones[ZONE_MOVABLE];
	return !test_bit(ZONE_NO_ALLOC, &zone->flags);
}

static int cram_pick_node(int src_nid)
{
	int best = NUMA_NO_NODE, best_dist = INT_MAX, nid;

	rcu_read_lock();
	for_each_node_mask(nid, cram_node_mask) {
		struct cram_node *cn = rcu_dereference(cram_nodes[nid]);
		int dist;

		if (!__cram_node_usable(cn))
			continue;
		dist = node_distance(src_nid, nid);
		if (dist < best_dist) {
			best_dist = dist;
			best = nid;
		}
	}
	rcu_read_unlock();

	return best;
}

bool cram_can_demote(int src_nid)
{
	if (node_isset(src_nid, cram_node_mask))
		return false;
	return cram_pick_node(src_nid) != NUMA_NO_NODE;
}

bool cram_folio_eligible(struct folio *folio)
{
	return !node_isset(folio_nid(folio), cram_node_mask) &&
	       folio_test_swapbacked(folio) && folio_test_anon(folio);
}

static void cram_zero_folio(struct folio *folio)
{
	unsigned int i;

	if (want_init_on_free())
		return;
	for (i = 0; i < folio_nr_pages(folio); i++)
		clear_highpage(folio_page(folio, i));
}

static struct folio *cram_alloc_folio(struct folio *src,
				      unsigned long private)
{
	gfp_t gfp = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) |
		    __GFP_NOMEMALLOC | __GFP_NOWARN | __GFP_THISNODE;
	int nid = cram_pick_node(folio_nid(src));

	if (nid == NUMA_NO_NODE)
		return NULL;
	return folio_alloc_node_private(gfp, folio_order(src), nid);
}

static void cram_put_new_folio(struct folio *folio, unsigned long private)
{
	cram_zero_folio(folio);
	folio_put(folio);
}

int cram_migrate_to(struct list_head *folios, enum migrate_mode mode,
		    enum migrate_reason reason, unsigned int *nr_succeeded)
{
	unsigned int nr = 0;
	int ret;

	ret = migrate_pages(folios, cram_alloc_folio, cram_put_new_folio, 0,
			    mode, reason, &nr);
	if (nr_succeeded)
		*nr_succeeded = nr;
	return ret;
}

static void cram_release_zone_withdrawal(struct cram_node *cn)
{
	struct zone *zone = &NODE_DATA(cn->nid)->node_zones[ZONE_MOVABLE];

	/* The zone outlives the cram_node that owns its withdrawal. */
	if (test_bit(ZONE_NO_ALLOC, &zone->flags))
		zone_clear_no_alloc(zone);
}

/**
 * cram_register() - donate physical region(s) to CRAM as a private node
 * @nid:         target NUMA node
 * @ranges:      perceived physical regions to donate (struct range, end-inclusive)
 * @n:           number of ranges
 * @features:    node memory feature mask; reclaim is required, while common
 *               placement and in-place userspace writes are forbidden
 *
 * CRAM owns the node lifecycle.  It hotplugs each range as a private node
 * with @features and onlines it movable.  The driver does no hotplug of its
 * own.  kswapd is started by the hotplug path because reclaim is required.
 *
 * Return: 0 on success.  -errno on failure (nothing left onlined on failure).
 */
int cram_register(int nid, const struct range *ranges, unsigned int n,
		  unsigned long features)
{
	struct cram_node *cn;
	unsigned int i, added = 0;
	int ret;

	if (!cram_valid_nid(nid) || !ranges || !n)
		return -EINVAL;
	if (!(features & NODE_MEMORY_FEAT_RECLAIM) ||
	    !(features & NODE_MEMORY_FEAT_WR_FENCE) ||
	    (features & NODE_MEMORY_FEAT_COMMON) ||
	    (features & ~NODE_MEMORY_FEAT_VALID))
		return -EINVAL;

	cn = kzalloc_obj(*cn);
	if (!cn) {
		ret = -ENOMEM;
		goto err;
	}
	cn->ranges = kmemdup(ranges, n * sizeof(*ranges), GFP_KERNEL);
	if (!cn->ranges) {
		ret = -ENOMEM;
		goto err;
	}
	cn->nr_ranges = n;
	cn->nid = nid;

	mutex_lock(&cram_mutex);

	/* Accretion / re-register onto an existing node is not yet supported. */
	if (rcu_access_pointer(cram_nodes[nid])) {
		mutex_unlock(&cram_mutex);
		ret = -EBUSY;
		goto err_ranges;
	}

	/*
	 * CRAM owns the node.  Hotplug each range as a private node (the
	 * features exclude NODE_MEMORY_FEAT_COMMON and include
	 * NODE_MEMORY_FEAT_WR_FENCE, so it is private and write-fenced) and
	 * online it movable.  On a failure roll back the ranges already added
	 * (fresh/empty, so the atomic ranges-offline succeeds).
	 */
	for (i = 0; i < n; i++) {
		ret = __add_memory_driver_managed(nid, ranges[i].start,
						  range_len(&ranges[i]),
						  "System RAM (cram)",
				MHP_MERGE_RESOURCE, MMOP_ONLINE_MOVABLE,
				features);
		if (ret) {
			if (added)
				offline_and_remove_memory_ranges(cn->ranges, added);
			mutex_unlock(&cram_mutex);
			goto err_ranges;
		}
		added++;
	}

	/*
	 * Publish the control node first, then arm the CRAM mask last.  Once
	 * the mask shows the node, cram_nodes[nid] is already valid.  Teardown
	 * clears the mask first, the mirror image.
	 */
	rcu_assign_pointer(cram_nodes[nid], cn);
	node_set(nid, cram_node_mask);
	mutex_unlock(&cram_mutex);
	return 0;

err_ranges:
	kfree(cn->ranges);
err:
	kfree(cn);
	return ret;
}
EXPORT_SYMBOL_GPL(cram_register);

/**
 * cram_unregister() - reclaim region(s) donated to CRAM (reverse of cram_register)
 * @nid:     the node
 * @ranges:  regions to remove.  Must exactly match the node's registered set.
 * @n:       number of ranges
 *
 * Evicts CRAM folios off the range(s) and removes the memory.  All-or-nothing
 * via offline_and_remove_memory_ranges().  On eviction failure returns -EBUSY
 * having removed nothing (node stays intact, retry the same set).  On success
 * this is the node's teardown (S1 supports whole-node removal only).  The
 * cram_node outlives removal and is freed afterwards.
 */
int cram_unregister(int nid, const struct range *ranges, unsigned int n)
{
	struct cram_node *cn;
	int ret;

	if (!cram_valid_nid(nid) || !ranges || !n)
		return -EINVAL;

	mutex_lock(&cram_mutex);
	cn = rcu_dereference_protected(cram_nodes[nid],
				       lockdep_is_held(&cram_mutex));
	if (!cn) {
		mutex_unlock(&cram_mutex);
		return -ENODEV;
	}
	/*
	 * S1: whole-node teardown only.  The set must match the registered
	 * regions exactly (same order, as the driver rebuilds them).  Per-region
	 * removal / accretion is a later refinement.
	 */
	if (n != cn->nr_ranges ||
	    memcmp(ranges, cn->ranges, n * sizeof(*ranges))) {
		mutex_unlock(&cram_mutex);
		return -EINVAL;
	}

	/*
	 * Atomic all-or-nothing.  A failure rolls back and removes nothing, so the
	 * node is left intact (still online + published) for the driver to retry.
	 * Hot-unplug isolates each range before migrating it, so new placements
	 * cannot land in the range being removed.
	 */
	ret = offline_and_remove_memory_ranges(cn->ranges, cn->nr_ranges);
	if (ret) {
		mutex_unlock(&cram_mutex);
		return ret;		/* The node remains intact. */
	}

	/* Success: tear down node bookkeeping. */
	node_clear(nid, cram_node_mask);
	rcu_assign_pointer(cram_nodes[nid], NULL);
	mutex_unlock(&cram_mutex);
	/* Drain reclaim-side readers before freeing cn. */
	synchronize_rcu();
	cram_release_zone_withdrawal(cn);

	kfree(cn->ranges);
	kfree(cn);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_unregister);

/**
 * cram_set_no_alloc() - withdraw or restore a CRAM node
 * @nid: CRAM node
 * @no_alloc: true withdraws the node, false restores it
 *
 * This is the provider's critical-low-memory signal.  ZONE_NO_ALLOC is the
 * source of truth and stays set until the provider clears it.  Withdrawing the
 * zone does not touch resident folios; reclaim spills to another CRAM node or
 * swap.
 */
int cram_set_no_alloc(int nid, bool no_alloc)
{
	struct cram_node *cn;
	struct zone *zone;
	int ret = 0;

	if (!cram_valid_nid(nid))
		return -ENODEV;

	mutex_lock(&cram_mutex);
	cn = rcu_dereference_protected(cram_nodes[nid],
				       lockdep_is_held(&cram_mutex));
	if (!cn) {
		mutex_unlock(&cram_mutex);
		return -ENODEV;
	}
	zone = &NODE_DATA(nid)->node_zones[ZONE_MOVABLE];

	if (no_alloc) {
		if (!test_bit(ZONE_NO_ALLOC, &zone->flags))
			ret = zone_set_no_alloc(zone);
	} else if (test_bit(ZONE_NO_ALLOC, &zone->flags)) {
		zone_clear_no_alloc(zone);
	}
	mutex_unlock(&cram_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(cram_set_no_alloc);

MODULE_DESCRIPTION("Compressed-RAM private-node anonymous-memory service");
MODULE_LICENSE("GPL");
