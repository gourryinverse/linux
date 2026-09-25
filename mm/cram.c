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
 * cram_register().  It reports usable capacity with cram_set_capacity().  CRAM
 * then resizes a balloon so allocatable capacity tracks the physical backing.
 * Reserved balloon pages are returned through the provider's trim callback.
 * cram_set_no_alloc() is the provider's critical-low admission gate.
 *
 * CRAM does not depend on swap/vswap.  Entry is by migration and resident CRAM
 * pages are ordinary anon LRU folios on the private node.
 */

#include <linux/balloon.h>
#include <linux/cram.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
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
#include <linux/workqueue.h>

/*
 * RECLAIM lets pressure on the CRAM node swap resident anonymous folios out.
 * WR_FENCE is set, so userspace writes copy the folio to common memory.
 * Incoming placement is owned by the explicit CRAM migration
 * path below, not by generic tiering.
 */
/* Maximum number of pages adjusted by one convergence iteration. */
#define CRAM_WMARK_CHUNK	(SZ_8M / PAGE_SIZE)

#define CRAM_BALLOON_BATCH	512

struct cram_node {
	int			nid;
	struct range		*ranges;	/* donated regions */
	unsigned int		nr_ranges;
	struct cram_ops		ops;		/* driver callbacks (by value) */
	void			*driver_data;	/* opaque, passed to ops callbacks */
	struct work_struct	wmark_work;	/* balloon convergence worker */
	struct balloon_dev_info	balloon;	/* reserved pages, held out of the buddy */
	unsigned long		nr_balloon;
	unsigned long		target_balloon;	/* convergence target */
	struct mutex		balloon_mutex;	/* serializes inflate/deflate + target */
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

	/*
	 * A cram_node is freed at unregister; the zone is not.  CRAM is the
	 * zone's sole owner, so release any outstanding driver withdrawal.
	 */
	if (test_bit(ZONE_NO_ALLOC, &zone->flags))
		zone_clear_no_alloc(zone);
}

/* Zero a reserved balloon page so the compressor sees a minimal footprint. */
static void cram_zero_range(unsigned long start_pfn, unsigned long nr_pages)
{
	unsigned long pfn;

	for (pfn = start_pfn; pfn < start_pfn + nr_pages; pfn++)
		clear_highpage(pfn_to_page(pfn));
}

/*
 * Release device backing for a contiguous range of reserved pages.  If the
 * provider cannot trim the range, zero it so the compressor can discard its
 * contents.  The pages are already out of the buddy allocator.
 *
 * A provider that needs retries or per-page error handling implements that
 * policy inside its callback; the core only needs an all-or-nothing result.
 */
static void cram_trim_range(struct cram_node *cn, unsigned long start_pfn,
			    unsigned long nr_pages)
{
	if (cn->ops.trim &&
	    !cn->ops.trim(cn->driver_data, start_pfn, nr_pages))
		return;
	cram_zero_range(start_pfn, nr_pages);
}

/*
 * A reserved page is being migrated.  Only reached because the balloon marks
 * its pages movable, which is the point: without it a convergence leaves the
 * node permanently fragmented with reservations that compaction cannot move,
 * and offline cannot evacuate them at all.
 *
 * Never blocks on balloon_mutex.  cram_balloon_inflate() allocates, allocation
 * can enter compaction, and compaction can arrive here -- so waiting would
 * recurse on the mutex the inflate already holds.
 */
static int cram_balloon_migratepage(struct balloon_dev_info *b_dev_info,
				    struct page *newpage, struct page *page,
				    enum migrate_mode mode)
{
	struct cram_node *cn = container_of(b_dev_info, struct cram_node,
					    balloon);
	unsigned long pfn = page_to_pfn(newpage);

	if (!mutex_trylock(&cn->balloon_mutex))
		return -EAGAIN;

	/*
	 * Off-node is the offline path deliberately evacuating us.  The
	 * reservation cannot follow: it exists to shrink THIS node, and
	 * nr_balloon is what the convergence target is measured against, so a
	 * page counted here but living elsewhere would overstate what has been
	 * given back.  -ENOENT tells the balloon the old page was released and
	 * the new one was not taken, which is exactly right -- the replacement
	 * becomes an ordinary page on whichever node it landed.
	 */
	if (page_to_nid(newpage) != page_to_nid(page)) {
		cn->nr_balloon--;
		mutex_unlock(&cn->balloon_mutex);
		return -ENOENT;
	}

	/* The reservation is only a reservation while the device is not backing it. */
	cram_trim_range(cn, pfn, 1);

	mutex_unlock(&cn->balloon_mutex);
	return 0;
}

/*
 * One pageblock's worth of pages, acquired whole.
 *
 * alloc_contig_range() is the acquisition primitive rather than the allocator
 * because a provider can withdraw its zone with zone_set_no_alloc(), and an
 * allocation would be refused by the flag its owner set.  ACR is PFN-addressed
 * -- it never walks a zonelist, so zone_allows_alloc() never sees it -- which is
 * the same reason unplug keeps working on a withdrawn zone.  It also migrates
 * residents out and takes the resulting free pages in one call.
 *
 * Pageblock rather than memory block.  The unit only has to be something ACR can
 * isolate, and a pageblock is the natural one; a memory block would quantise
 * capacity to 128MB-2GB for no gain.
 *
 * Return: pages added to the balloon, or 0 if the range could not be cleared.
 */
static unsigned long cram_acquire_block(struct cram_node *cn,
					unsigned long start_pfn,
					unsigned long nr, acr_flags_t acr)
{
	LIST_HEAD(pages);
	unsigned long pfn;

	/*
	 * Cheap skip for a block we already hold.  Wrong only after a
	 * reservation has been migrated within the node, and then merely
	 * wasteful: ACR moves the balloon pages aside and we take the block
	 * anyway.
	 */
	if (PageOffline(pfn_to_page(start_pfn)))
		return 0;

	if (alloc_contig_range(start_pfn, start_pfn + nr,
			       acr | ACR_FLAGS_PRIVATE, GFP_KERNEL))
		return 0;

	cram_trim_range(cn, start_pfn, nr);
	for (pfn = start_pfn; pfn < start_pfn + nr; pfn++)
		list_add(&pfn_to_page(pfn)->lru, &pages);
	cn->nr_balloon += balloon_page_list_enqueue(&cn->balloon, &pages);

	return nr;
}

/*
 * Takes pageblocks off the node and holds them, so placement cannot use that
 * capacity.  Clearing a block migrates its residents, which on an overcommitted
 * node means driving them out to physical swap (the node has RECLAIM) -- that is
 * how the reservation is paid for.  Reserved pages go to the driver's trim
 * callback, or are zeroed, so the device can reclaim their physical backing.
 *
 * Walks the node's own donated ranges, so it is bounded by the node and never
 * touches anyone else's memory.  A block that will not clear is skipped rather
 * than retried; the caller's convergence loop comes back round.
 *
 * Return: number of pages actually inflated (may be less than requested when
 * the node cannot free any more).
 */
static unsigned long cram_balloon_inflate(struct cram_node *cn,
					  unsigned long nr_pages,
					  acr_flags_t acr)
{
	const unsigned long blk = pageblock_nr_pages;
	unsigned long inflated = 0;
	unsigned int r;

	mutex_lock(&cn->balloon_mutex);

	for (r = 0; r < cn->nr_ranges && inflated < nr_pages; r++) {
		unsigned long pfn = PFN_UP(cn->ranges[r].start);
		unsigned long end = PFN_DOWN(cn->ranges[r].end + 1);

		pfn = ALIGN(pfn, blk);
		for (; pfn + blk <= end && inflated < nr_pages; pfn += blk) {
			inflated += cram_acquire_block(cn, pfn, blk, acr);
			cond_resched();
		}
	}

	mutex_unlock(&cn->balloon_mutex);
	return inflated;
}

/* A short deflate is retried by the convergence worker. */
static unsigned long cram_balloon_deflate(struct cram_node *cn,
					  unsigned long nr_pages)
{
	unsigned long deflated = 0, i;

	/*
	 * A page isolated for migration is temporarily off the balloon's list,
	 * so a pass can come up short while the balloon is not actually empty.
	 * The convergence worker retries; hot-unplug migrates any remainder.
	 */
	mutex_lock(&cn->balloon_mutex);

	/* Batched: the dequeue holds a lock with interrupts off. */
	while (deflated < nr_pages) {
		struct page *page, *next;
		LIST_HEAD(batch);

		i = balloon_page_list_dequeue(&cn->balloon, &batch,
					      min(nr_pages - deflated,
						  (unsigned long)CRAM_BALLOON_BATCH));
		if (!i)
			break;
		cn->nr_balloon -= i;
		deflated += i;

		list_for_each_entry_safe(page, next, &batch, lru) {
			list_del(&page->lru);
			__free_page(page);	/* held refcounted */
		}
		cond_resched();
	}

	mutex_unlock(&cn->balloon_mutex);
	return deflated;
}

/* Total number of pages donated to the node. */
static unsigned long cram_perceived_pages(const struct cram_node *cn)
{
	unsigned long pages = 0;
	unsigned int i;

	for (i = 0; i < cn->nr_ranges; i++)
		pages += range_len(&cn->ranges[i]) >> PAGE_SHIFT;
	return pages;
}

/*
 * cram_wmark_work_fn() - balloon convergence worker.
 *
 * Drives nr_balloon toward target_balloon, re-reading the target each iteration
 * so rapid capacity updates coalesce to the latest snapshot.  Inflation first
 * migrates resident folios.  When the provider has withdrawn the zone, it may
 * also reclaim them to swap after migration fails.  The provider owns that
 * withdrawal and explicitly clears it after recovery.
 */
static void cram_wmark_work_fn(struct work_struct *work)
{
	struct cram_node *cn = container_of(work, struct cram_node, wmark_work);
	unsigned long target = 0;

again:
	for (;;) {
		unsigned long cur = READ_ONCE(cn->nr_balloon);

		target = READ_ONCE(cn->target_balloon);
		if (cur < target) {
			unsigned long want = min(CRAM_WMARK_CHUNK, target - cur);

			/*
			 * Migration alone is the polite first attempt.  Re-enter at
			 * this rung each round because a range that did not clear last
			 * time may clear now.
			 */
			if (cram_balloon_inflate(cn, want, ACR_FLAGS_NONE))
				goto next;

			/*
			 * Polite migration is spent.  In steady state that is
			 * the right place to stop: the node is full of memory
			 * that is in use, and the driver can wait.
			 */
			if (__cram_node_usable(cn))
				break;

			/* The provider says the remaining capacity is unavailable now. */
			if (cram_balloon_inflate(cn, want, ACR_FLAGS_RECLAIM))
				goto next;
			break;
		} else if (cur > target) {
			cram_balloon_deflate(cn, min(CRAM_WMARK_CHUNK, cur - target));
		} else {
			break;		/* converged */
		}
next:
		cond_resched();
	}

	/* A concurrent capacity update may have changed the target as we stopped. */
	mutex_lock(&cn->balloon_mutex);
	if (target != cn->target_balloon) {
		mutex_unlock(&cn->balloon_mutex);
		goto again;
	}
	mutex_unlock(&cn->balloon_mutex);
}

/**
 * cram_register() - donate physical region(s) to CRAM as a private node
 * @nid:         target NUMA node
 * @ranges:      perceived physical regions to donate (struct range, end-inclusive)
 * @n:           number of ranges
 * @features:    node memory feature mask; reclaim is required, while common
 *               placement and in-place userspace writes are forbidden
 * @ops:         driver callbacks.  ops.owner (= THIS_MODULE) pins the module for
 *               the node's lifetime.  Stored by value.
 * @driver_data: the driver's per-node object, the ops callback cookie and np
 *               owner.  Opaque to CRAM (keyed operations use @nid).
 *
 * CRAM owns the node lifecycle.  It hotplugs each range as a private node
 * with @features and onlines it movable.  The driver does no hotplug of its
 * own.  kswapd is started by the hotplug path because reclaim is required.
 *
 * Return: 0 on success.  -errno on failure (nothing left onlined on failure).
 */
int cram_register(int nid, const struct range *ranges, unsigned int n,
		  unsigned long features, struct cram_ops ops,
		  void *driver_data)
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

	/* Pin the driver module for the node's lifetime (NULL = built-in). */
	if (!try_module_get(ops.owner))
		return -EBUSY;

	cn = kzalloc_obj(*cn);
	if (!cn) {
		ret = -ENOMEM;
		goto err_module;
	}
	cn->ranges = kmemdup(ranges, n * sizeof(*ranges), GFP_KERNEL);
	if (!cn->ranges) {
		ret = -ENOMEM;
		goto err_free;
	}
	cn->nr_ranges = n;
	cn->ops = ops;
	cn->driver_data = driver_data;
	cn->nid = nid;
	INIT_WORK(&cn->wmark_work, cram_wmark_work_fn);
	balloon_devinfo_init(&cn->balloon);
	cn->balloon.migratepage = cram_balloon_migratepage;
	/*
	 * The reservation is a real capacity reduction -- the device is no
	 * longer backing these pages -- so MemTotal should say so.
	 */
	cn->balloon.adjust_managed_page_count = true;
	mutex_init(&cn->balloon_mutex);

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
err_free:
	kfree(cn);
err_module:
	module_put(ops.owner);
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
	struct module *owner;
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
	 * Point the balloon target at 0 so a running convergence worker deflates
	 * (never re-inflates) under us.  Then release the balloon so offline can
	 * migrate everything off the ranges.
	 */
	WRITE_ONCE(cn->target_balloon, 0);
	cram_balloon_deflate(cn, ULONG_MAX);

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
	owner = cn->ops.owner;
	node_clear(nid, cram_node_mask);
	rcu_assign_pointer(cram_nodes[nid], NULL);
	mutex_unlock(&cram_mutex);
	/* Drain reclaim-side readers before freeing cn. */
	synchronize_rcu();

	/*
	 * The node is now unpublished and driver controls are excluded by
	 * cram_mutex, so the worker cannot be requeued.
	 */
	cancel_work_sync(&cn->wmark_work);

	cram_release_zone_withdrawal(cn);

	kfree(cn->ranges);
	kfree(cn);
	module_put(owner);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_unregister);

/**
 * cram_set_capacity() - report the node's usable capacity
 * @nid: CRAM node
 * @nr_pages: number of pages the provider can currently back
 *
 * Pages beyond the reported capacity are reserved in the balloon.  The value
 * is capped at the total size of the donated ranges and convergence is
 * asynchronous.  The provider separately controls allocation admission with
 * cram_set_no_alloc().
 */
int cram_set_capacity(int nid, unsigned long nr_pages)
{
	struct cram_node *cn;
	unsigned long total;

	if (!cram_valid_nid(nid))
		return -ENODEV;

	mutex_lock(&cram_mutex);
	cn = rcu_dereference_protected(cram_nodes[nid],
				       lockdep_is_held(&cram_mutex));
	if (!cn) {
		mutex_unlock(&cram_mutex);
		return -ENODEV;
	}

	mutex_lock(&cn->balloon_mutex);
	total = cram_perceived_pages(cn);
	WRITE_ONCE(cn->target_balloon, total - min(nr_pages, total));
	/* Convergence can reclaim, so keep it off the shared system_wq. */
	queue_work(system_long_wq, &cn->wmark_work);
	mutex_unlock(&cn->balloon_mutex);
	mutex_unlock(&cram_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_set_capacity);

/**
 * cram_set_no_alloc() - withdraw or restore a CRAM node
 * @nid: CRAM node
 * @no_alloc: true withdraws the node, false restores it
 *
 * This is the provider's critical-low-memory signal.  ZONE_NO_ALLOC is the
 * source of truth and stays set until the provider clears it.  Withdrawing the
 * zone does not touch the balloon or resident folios; reclaim spills to another
 * CRAM node or swap.
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

	mutex_lock(&cn->balloon_mutex);
	if (no_alloc) {
		if (!test_bit(ZONE_NO_ALLOC, &zone->flags))
			ret = zone_set_no_alloc(zone);
	} else if (test_bit(ZONE_NO_ALLOC, &zone->flags)) {
		zone_clear_no_alloc(zone);
	}
	mutex_unlock(&cn->balloon_mutex);
	mutex_unlock(&cram_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(cram_set_no_alloc);

MODULE_DESCRIPTION("Compressed-RAM private-node anonymous-memory service");
MODULE_LICENSE("GPL");
