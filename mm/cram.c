// SPDX-License-Identifier: GPL-2.0
/*
 * mm/cram.c - Compressed RAM / private node memory management
 *
 * Manages folios demoted to N_MEMORY_PRIVATE nodes ("CRAM" nodes) via the
 * standard kernel LRU.  A CRAM folio maps present read-only so reads are
 * zero-copy.  The device decompresses in place.  A write promotes the folio
 * back to DRAM.  Private anonymous folios are the first tier; a write COWs off
 * via the do_wp_page() path.  The device also nominates hot pages for proactive
 * promotion.  See cram_report_hot_pages().
 *
 * A driver donates physical regions and registers as a node's CRAM owner with
 * cram_register().  It advertises a PERCEIVED size at a configured compression
 * ratio (zratio).  As the achieved ratio drifts the driver reports it via
 * cram_set_compression_ratio().  CRAM then resizes a balloon so EFFECTIVE
 * capacity tracks the real physical backing.  Reserved balloon pages are
 * returned to the device via the trim callback.  cram_allow_allocation() is
 * the sticky admission gate.
 *
 * CRAM does not depend on swap/vswap.  Entry is by migration and resident CRAM
 * pages are ordinary anon LRU folios on the private node.
 */

#include <linux/atomic.h>
#include <linux/cpuset.h>
#include <linux/cram.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/jiffies.h>
#include <linux/jump_label.h>
#include <linux/highmem.h>
#include <linux/memory-tiers.h>
#include <linux/memory_hotplug.h>
#include <linux/list.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/mmzone.h>
#include <linux/mutex.h>
#include <linux/nodemask.h>
#include <linux/node_private.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/signal.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include <linux/memcontrol.h>
#include <linux/mm_inline.h>

#include <linux/debugfs.h>
#include "internal.h"

/*
 * Node opt-ins for a CRAM node.  The reclaim and hot-unplug service CAPs let
 * kswapd / direct reclaim write resident CRAM folios back to physical swap and
 * let teardown migrate them off.  The WRITE_FENCE POLICY maps CRAM folios
 * read-only so a write must COW-promote off the device (folio_must_cow()).
 */
#define CRAM_NP_CAPS	(NODE_PRIVATE_CAP_RECLAIM | NODE_PRIVATE_CAP_HOTUNPLUG | \
			 NODE_PRIVATE_POLICY_WRITE_FENCE)

/* Pages per proactive-reclaim chunk in the watermark-low loop */
#define CRAM_WMARK_CHUNK	(SZ_8M / PAGE_SIZE)

/* Pages handed to the driver's trim callback per call, and -EAGAIN retry cap. */
#define CRAM_TRIM_BATCH		512
#define CRAM_TRIM_RETRIES	8

/*
 * Bounded ring of device-nominated hot pfns awaiting proactive promotion.
 * Overflow is dropped (the device re-reports later); sizing it generously keeps
 * a single report_hot_pages() burst from spilling.
 */
#define CRAM_PROMOTE_RING	1024

/*
 * Gates folio_is_cram() (<linux/cram.h>): enabled while >= 1 CRAM node is
 * registered, so a CONFIG_CRAM=y kernel with no CRAM device pays a patched
 * NOP at every hook instead of a node lookup.
 */
DEFINE_STATIC_KEY_FALSE(cram_enabled);

/* Observability counters, exposed via debugfs */
static atomic_long_t cram_cnt_demote = ATOMIC_LONG_INIT(0);
static atomic_long_t cram_cnt_promote = ATOMIC_LONG_INIT(0);
static atomic_long_t cram_cnt_promote_fail = ATOMIC_LONG_INIT(0);

struct cram_node {
	bool			alloc_allowed;	/* sticky: driver permits demotions */
	bool			migration_blocked; /* external block: teardown / [TEST] */
	bool			converge_block;	/* block_alloc convergence (worker) */
	int			nid;
	u32			zratio;		/* hw compression ratio, per-mille N:1 */
	u32			current_ratio;	/* achieved ratio, per-mille */
	struct range		*ranges;	/* donated regions */
	unsigned int		nr_ranges;
	struct node_private	np;		/* backs the node until removal */
	refcount_t		refcount;
	struct cram_ops		ops;		/* driver callbacks (by value) */
	void			*driver_data;	/* opaque, passed to ops callbacks */
	struct work_struct	wmark_work;	/* balloon convergence worker */
	struct list_head	balloon_pages;	/* reserved (held) pages */
	unsigned long		nr_balloon;
	unsigned long		target_balloon;	/* convergence target */
	struct mutex		balloon_mutex;	/* serializes inflate/deflate + target */
	struct work_struct	promote_work;	/* drains the hot-page nomination ring */
	spinlock_t		promote_lock;	/* guards promote_ring/promote_nr */
	unsigned int		promote_nr;	/* pending nominations in the ring */
	unsigned long		promote_ring[CRAM_PROMOTE_RING]; /* device-nominated pfns */
};

static struct cram_node __rcu *cram_nodes[MAX_NUMNODES];
static DEFINE_MUTEX(cram_mutex);

/*
 * The set of registered CRAM nodes, mirroring cram_nodes[] as a bitmap.  Bits
 * are flipped under cram_mutex at register/unregister and read locklessly, like
 * node_states.  node_is_cram() / folio_is_cram() test a bit.  CRAM's own walks
 * iterate it directly (for_each_node_mask) instead of filtering the wider
 * N_MEMORY_PRIVATE set.  The array stays the authority for the node's control
 * state (refcount, alloc_allowed, ...).  The mask just answers "is it CRAM".
 */
nodemask_t cram_node_mask __read_mostly;

static inline bool cram_valid_nid(int nid)
{
	return nid >= 0 && nid < MAX_NUMNODES;
}

static inline struct cram_node *get_cram_node(int nid)
{
	struct cram_node *cn;

	if (!cram_valid_nid(nid))
		return NULL;

	rcu_read_lock();
	cn = rcu_dereference(cram_nodes[nid]);
	if (cn && !refcount_inc_not_zero(&cn->refcount))
		cn = NULL;
	rcu_read_unlock();

	return cn;
}

static inline void put_cram_node(struct cram_node *cn)
{
	if (cn)
		refcount_dec(&cn->refcount);
}

static void cram_set_migration_blocked(int nid, bool blocked)
{
	struct cram_node *cn;

	rcu_read_lock();
	cn = rcu_dereference(cram_nodes[nid]);
	if (cn)
		WRITE_ONCE(cn->migration_blocked, blocked);
	rcu_read_unlock();
}

/*
 * __folio_is_cram() - out-of-line body behind the cram_enabled static key.
 *
 * Callers use folio_is_cram() (<linux/cram.h>), which NOPs this out when no
 * CRAM node is registered.  This is just a nodemask test (see node_is_cram()).
 * It is out-of-line only to keep folio_nid() (and thus <linux/mm.h>) out of
 * the header.
 */
bool __folio_is_cram(struct folio *folio)
{
	return node_isset(folio_nid(folio), cram_node_mask);
}

/*
 * __cram_node_usable() - may CRAM node @cn receive a demotion / store now?
 *
 * Usable requires the driver's sticky permission (alloc_allowed, set via
 * cram_allow_allocation()) and no back-pressure from either block source.
 * migration_blocked covers the unregister teardown and the [TEST] gate.
 * converge_block covers a block_alloc convergence.  The two blocks are distinct
 * so the balloon worker only ever clears its own (converge_block) and can never
 * lift the teardown fence.  Caller holds RCU/ref.
 */
static bool __cram_node_usable(const struct cram_node *cn)
{
	return cn && READ_ONCE(cn->alloc_allowed) &&
	       !READ_ONCE(cn->migration_blocked) &&
	       !READ_ONCE(cn->converge_block);
}

/*
 * cram_pick_node() - choose which CRAM node should receive a folio from @src_nid.
 *
 * Returns the nearest usable CRAM private node to @src_nid, or NUMA_NO_NODE if
 * none can take it right now.  Usable means registered, allocation-allowed, and
 * not back-pressured.  CRAM owns placement.  Callers hand over folios and let
 * this decide.
 */
static int cram_pick_node(int src_nid)
{
	int best = NUMA_NO_NODE, best_dist = INT_MAX, nid;

	rcu_read_lock();
	for_each_node_mask(nid, cram_node_mask) {
		int dist;

		if (!__cram_node_usable(rcu_dereference(cram_nodes[nid])))
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

/*
 * cram_can_demote() - is CRAM available to absorb folios reclaimed from @src_nid?
 *
 * The vmscan demote-in hook uses this to decide whether to collect folios for
 * CRAM at all.  It does not pick a destination.  cram_pick_node() does that,
 * per folio.  Reclaim from a private node itself is the writeback-to-physical
 * path, so never re-cram from one.
 */
bool cram_can_demote(int src_nid)
{
	/*
	 * Reclaim from a CRAM node itself is the writeback-to-physical path
	 * (kswapd swaps its folios out), never a re-cram.  Test the CRAM set
	 * specifically, not N_MEMORY_PRIVATE.  Other private nodes may belong
	 * to unrelated services and are not excluded here.
	 */
	if (node_is_cram(src_nid))
		return false;

	return cram_pick_node(src_nid) != NUMA_NO_NODE;
}

/*
 * cram_folio_eligible - may this folio be demoted into CRAM?
 *
 * Private anonymous folios qualify: mapped read-only on the tier and evicted by
 * swapping the resident folio out (the node has CAP_RECLAIM); a write COWs back
 * to DRAM (do_wp_page / folio_must_cow).  shmem/tmpfs is excluded (folio_test_
 * anon is false, incl. MAP_ANONYMOUS shared) -- it rides zswap.
 *
 * Never re-cram an already-CRAM folio.
 */
bool cram_folio_eligible(struct folio *folio)
{
	if (folio_is_cram(folio))
		return false;
	/* Only private anonymous folios demote to CRAM; shmem rides zswap. */
	if (folio_test_swapbacked(folio))
		return folio_test_anon(folio);
	return false;
}

static void cram_zero_folio(struct folio *folio)
{
	unsigned int i, nr = folio_nr_pages(folio);

	if (want_init_on_free())
		return;

	for (i = 0; i < nr; i++)
		clear_highpage(folio_page(folio, i));
}

static struct folio *alloc_cram_folio(struct folio *src, unsigned long private)
{
	unsigned int order = folio_order(src);
	gfp_t gfp = GFP_HIGHUSER_MOVABLE | __GFP_KSWAPD_RECLAIM |
		     __GFP_NOWARN | __GFP_NORETRY;
	int nid;

	/* CRAM picks the destination per folio (nearest usable node). */
	nid = cram_pick_node(folio_nid(src));
	if (nid == NUMA_NO_NODE)
		return NULL;

	if (order)
		gfp |= __GFP_COMP;

	return folio_alloc_node_private(gfp, order, nid);
}

static void cram_put_new_folio(struct folio *folio, unsigned long private)
{
	cram_zero_folio(folio);
	folio_put(folio);
}

/*
 * cram_migrate_to() - demote a batch of folios into CRAM.
 *
 * Called from the reclaim path (shrink_folio_list cram hook) with a list of
 * isolated folios the caller already filtered with cram_folio_eligible()
 * (private anon, swap-backed, never already-CRAM).  CRAM owns placement.  Each
 * folio is migrated onto the nearest usable CRAM node (alloc_cram_folio ->
 * cram_pick_node), so the caller never specifies a destination.  Migration
 * installs the folios present read-only via remove_migration_pte()/_pmd().
 * Folios that don't fit stay on the list for the caller to fall back to swap.
 */
int cram_migrate_to(struct list_head *demote_folios, int src_nid,
		    enum migrate_mode mode, enum migrate_reason reason,
		    unsigned int *nr_succeeded)
{
	unsigned int nr_success = 0;
	int ret;

	ret = migrate_pages(demote_folios, alloc_cram_folio, cram_put_new_folio,
			    (unsigned long)src_nid, mode, reason, &nr_success);
	atomic_long_add(nr_success, &cram_cnt_demote);

	if (nr_succeeded)
		*nr_succeeded = nr_success;
	return ret;
}

/*
 * Anonymous CRAM folios promote out of the tier via the core COW path.  A write
 * fault reaches wp_page_copy() because wp_can_reuse_anon_folio()
 * refuses CRAM folios (like KSM), COWing into a fresh anon DRAM folio.  The
 * device also nominates hot resident folios for proactive promotion, migrated
 * in place below (cram_promote_folio_to / cram_report_hot_pages).
 */

/* Nearest DRAM (N_MEMORY) node to a CPU-less CRAM node, for proactive promotion. */
static int cram_nearest_dram(int cram_nid)
{
	int best = NUMA_NO_NODE, best_dist = INT_MAX, nid;

	for_each_node_state(nid, N_MEMORY) {
		int dist = node_distance(cram_nid, nid);

		if (dist < best_dist) {
			best_dist = dist;
			best = nid;
		}
	}
	return best;
}

/*
 * cram_promote_folio_to() - migrate one resident CRAM folio to DRAM in place.
 *
 * The read-side inverse of demotion.  The folio stays readable throughout (rmap
 * repoints the PTEs), no write required.  Driven by device-reported hotness.
 * Best-effort.  Any transient failure is dropped and the device re-reports.
 *
 * Consumes one reference on @folio (the caller's lookup ref), so migrate_pages()
 * sees only the isolation reference.
 */
static void cram_promote_folio_to(struct folio *folio, int dst_nid)
{
	struct migration_target_control mtc = {
		.nid = dst_nid,
		.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_NOWARN,
		.reason = MR_NUMA_MISPLACED,
	};
	LIST_HEAD(list);
	int ret;

	/* Share the system promotion budget (numa_balancing_promote_rate_limit). */
	if (numa_promotion_rate_limited(dst_nid, folio_nr_pages(folio))) {
		folio_put(folio);
		return;
	}

	/* A just-demoted folio may still be on a per-cpu LRU add-batch. */
	if (!folio_test_lru(folio)) {
		lru_add_drain();
		if (!folio_test_lru(folio))
			lru_add_drain_all();
	}
	if (!folio_isolate_lru(folio)) {
		atomic_long_inc(&cram_cnt_promote_fail);
		folio_put(folio);
		return;
	}
	node_stat_mod_folio(folio, NR_ISOLATED_ANON + folio_is_file_lru(folio),
			    folio_nr_pages(folio));
	list_add(&folio->lru, &list);
	folio_put(folio);		/* migrate_pages() works off the isolation ref */

	ret = migrate_pages(&list, alloc_migration_target, NULL,
			    (unsigned long)&mtc, MIGRATE_SYNC, MR_NUMA_MISPLACED,
			    NULL);
	if (ret && !list_empty(&list))
		putback_movable_pages(&list);
	if (ret)
		atomic_long_inc(&cram_cnt_promote_fail);
	else
		atomic_long_inc(&cram_cnt_promote);
}

/*
 * cram_promote_work_fn() - drain the hot-page nomination ring.
 *
 * Snapshots the pending pfns under the ring lock, then resolves and promotes
 * each outside the lock.  pfns that are gone, already promoted, or not a CRAM
 * folio on this node are silently skipped, best-effort per the
 * report_hot_pages contract.
 */
static void cram_promote_work_fn(struct work_struct *work)
{
	struct cram_node *cn = container_of(work, struct cram_node, promote_work);
	unsigned long *batch;
	unsigned int i, n;
	int dst;

	batch = kmalloc_array(CRAM_PROMOTE_RING, sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return;		/* dropped; the device re-reports later */

	spin_lock(&cn->promote_lock);
	n = cn->promote_nr;
	memcpy(batch, cn->promote_ring, n * sizeof(batch[0]));
	cn->promote_nr = 0;
	spin_unlock(&cn->promote_lock);

	dst = n ? cram_nearest_dram(cn->nid) : NUMA_NO_NODE;
	if (dst == NUMA_NO_NODE) {
		kfree(batch);
		return;
	}

	for (i = 0; i < n; i++) {
		unsigned long pfn = batch[i];
		struct page *page;
		struct folio *folio;

		if (!pfn_valid(pfn))
			continue;
		page = pfn_to_online_page(pfn);
		if (!page)
			continue;
		folio = page_folio(page);
		if (!folio_try_get(folio))
			continue;
		/*
		 * Must still be a CRAM folio on this node (not freed/promoted).
		 * cram_promote_folio_to() consumes the ref.  The skip-path drops it.
		 */
		if (folio_nid(folio) == cn->nid && folio_is_cram(folio))
			cram_promote_folio_to(folio, dst);
		else
			folio_put(folio);

		if (need_resched())
			cond_resched();
	}
	kfree(batch);
}

/**
 * cram_promote_pagecache() - migrate a resident CRAM file folio to DRAM in place
 * @mapping: the file mapping
 * @index: page index within @mapping
 * @nowait: if true, do not block on migration
 *
 * Migrates the resident CRAM file folio at (@mapping, @index) off the tier to
 * DRAM, in place (rmap repoints every mapper).  This is the action behind the
 * file write-fence gates (filemap/memory/mprotect).  A byte writer promotes the
 * folio off the read-only device tier before touching it.  One-shot.  Never
 * serves the folio.
 *
 * Return: 0 when promoted, already promoted by a racer, or gone.  -EAGAIN on a
 * transient isolate/migrate failure, which the caller retries unless @nowait.
 */
int cram_promote_pagecache(struct address_space *mapping, pgoff_t index,
			   bool nowait)
{
	struct migration_target_control mtc = {
		.nid = numa_node_id(),	/* CRAM nodes are CPU-less: caller node is DRAM */
		.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_NOWARN,
		.reason = MR_NUMA_MISPLACED,
	};
	struct folio *folio;
	LIST_HEAD(list);
	int ret;

	/*
	 * Raw lookup.  filemap_get_folio() routes through the acquire gate, which
	 * calls back here, so use filemap_get_entry() to avoid the recursion.
	 */
	folio = filemap_get_entry(mapping, index);
	if (!folio || xa_is_value(folio))
		return 0;		/* gone / shadow entry: nothing resident */
	if (!folio_is_cram(folio)) {
		folio_put(folio);
		return 0;		/* already promoted by a racer */
	}
	if (nowait) {
		folio_put(folio);
		return -EAGAIN;		/* migration may block */
	}

	/*
	 * A just-demoted folio sits on a per-cpu LRU add-batch and is not yet
	 * isolatable.  Drain this cpu's batch first (cheap).  The demotion may have
	 * run on another cpu, so on a miss drain all cpus to flush it
	 * deterministically, with no retry-count guesswork.
	 */
	if (!folio_test_lru(folio)) {
		lru_add_drain();
		if (!folio_test_lru(folio))
			lru_add_drain_all();
	}
	if (!folio_isolate_lru(folio)) {
		/* Concurrently isolated (reclaim/compaction): transient, caller retries. */
		atomic_long_inc(&cram_cnt_promote_fail);
		folio_put(folio);
		return -EAGAIN;
	}
	node_stat_mod_folio(folio, NR_ISOLATED_ANON + folio_is_file_lru(folio),
			    folio_nr_pages(folio));
	list_add(&folio->lru, &list);
	folio_put(folio);		/* migrate_pages() works off the isolation ref */

	ret = migrate_pages(&list, alloc_migration_target, NULL,
			    (unsigned long)&mtc, MIGRATE_SYNC, MR_NUMA_MISPLACED,
			    NULL);
	if (ret && !list_empty(&list))
		putback_movable_pages(&list);
	if (ret) {
		atomic_long_inc(&cram_cnt_promote_fail);
		return -EAGAIN;
	}
	atomic_long_inc(&cram_cnt_promote);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_promote_pagecache);

/* Zero a reserved balloon page so the compressor sees a minimal footprint. */
static void cram_zero_page(unsigned long pfn)
{
	clear_highpage(pfn_to_page(pfn));
}

/*
 * cram_trim_batch() - release device backing for a batch of just-reserved pages.
 *
 * The pages are held by the balloon (free from buddy's view), so the device may
 * drop their compression-cache backing.  Honors the trim contract.  0 means
 * done.  -EAGAIN means retry the pages still marked failed (result[i] != 0).
 * -EBUSY means zero the failed pages instead.  When no trim callback is
 * registered, or retries are exhausted, CRAM zeroes the remaining pages itself.
 */
static void cram_trim_batch(struct cram_node *cn, unsigned long *pfns,
			    int *result, unsigned int n)
{
	unsigned int attempt, i, j;

	if (!cn->ops.trim) {
		for (i = 0; i < n; i++)
			cram_zero_page(pfns[i]);
		return;
	}

	for (attempt = 0; attempt < CRAM_TRIM_RETRIES && n; attempt++) {
		int ret;

		memset(result, 0, n * sizeof(*result));
		ret = cn->ops.trim(cn->driver_data, pfns, result, n);
		if (ret == 0)
			return;
		if (ret == -EBUSY) {
			for (i = 0; i < n; i++)
				if (result[i])
					cram_zero_page(pfns[i]);
			return;
		}
		/* -EAGAIN (or unexpected): compact the failed pages and retry. */
		for (i = 0, j = 0; i < n; i++)
			if (result[i])
				pfns[j++] = pfns[i];
		n = j;
	}

	/* Retries exhausted: fall back to zeroing whatever is still untrimmed. */
	for (i = 0; i < n; i++)
		cram_zero_page(pfns[i]);
}

/**
 * cram_balloon_inflate() - reserve pages on the CRAM node (reduce usable capacity)
 * @nid: CRAM private node
 * @nr_pages: pages to reserve
 *
 * Allocates pages on the node and holds them, so cram_migrate_to() cannot use
 * that capacity.  This is the proactive-reclaim + async-trim mechanism that
 * drives resident CRAM folios out to physical swap (the node has CAP_RECLAIM)
 * to satisfy the reservation.  Reserved pages are handed to the driver's trim
 * callback (or zeroed) so the device can reclaim their physical backing.
 *
 * The reclaiming allocation is node-local.  With __GFP_THISNODE a private-node
 * allocation uses ZONELIST_PRIVATE_NOFALLBACK, so the direct reclaim it drives
 * stays on @nid.  It swaps this node's own folios out and never walks the DRAM
 * fallback, which would demote DRAM folios back into the node and refill it as
 * fast as we reserve it.  Reclaim of a CRAM node never re-crams (do_cram_pass
 * == false).  When the node has nothing left to reclaim the allocation returns
 * NULL and we stop.  That is the cutoff that keeps the balloon node-local.
 *
 * Interruptible.  A large inflate can spend a long time reclaiming/swapping, so
 * it bails on a fatal signal and stays killable.
 *
 * Return: number of pages actually inflated (may be less than requested when
 * the node cannot free any more).
 */
static unsigned long cram_balloon_inflate(int nid, unsigned long nr_pages)
{
	gfp_t gfp = GFP_HIGHUSER_MOVABLE | __GFP_THISNODE | __GFP_NOWARN;
	unsigned long inflated = 0, i;
	unsigned long *trim_pfns;
	unsigned int nbatch = 0;
	int *trim_res;
	struct cram_node *cn;
	LIST_HEAD(new_pages);

	cn = get_cram_node(nid);
	if (!cn)
		return 0;

	/* Trim scratch: on OOM, reservations still proceed (just untrimmed). */
	trim_pfns = kmalloc_array(CRAM_TRIM_BATCH, sizeof(*trim_pfns), GFP_KERNEL);
	trim_res = kmalloc_array(CRAM_TRIM_BATCH, sizeof(*trim_res), GFP_KERNEL);

	mutex_lock(&cn->balloon_mutex);

	for (i = 0; i < nr_pages; i++) {
		struct folio *folio;

		if (fatal_signal_pending(current))
			break;	/* stay killable across a long reclaiming inflate */
		folio = folio_alloc_node_private(gfp, 0, nid);
		if (!folio)
			break;	/* node fully reserved, nothing left to reclaim */
		list_add(&folio->lru, &new_pages);
		inflated++;

		/* Drop device backing for the reserved pages (trim, or zero). */
		if (trim_pfns && trim_res) {
			trim_pfns[nbatch++] = folio_pfn(folio);
			if (nbatch == CRAM_TRIM_BATCH) {
				cram_trim_batch(cn, trim_pfns, trim_res, nbatch);
				nbatch = 0;
			}
		}
		if (!(i & 0x3ff))
			cond_resched();
	}
	if (nbatch)
		cram_trim_batch(cn, trim_pfns, trim_res, nbatch);

	list_splice(&new_pages, &cn->balloon_pages);
	cn->nr_balloon += inflated;

	mutex_unlock(&cn->balloon_mutex);
	put_cram_node(cn);
	kfree(trim_pfns);
	kfree(trim_res);
	return inflated;
}

/**
 * cram_balloon_deflate() - release reserved pages back to the CRAM node
 * @nid: CRAM private node
 * @nr_pages: pages to release
 *
 * Return: number of pages actually deflated.
 */
static unsigned long cram_balloon_deflate(int nid, unsigned long nr_pages)
{
	struct cram_node *cn;
	unsigned long deflated = 0, i;

	cn = get_cram_node(nid);
	if (!cn)
		return 0;

	mutex_lock(&cn->balloon_mutex);

	for (i = 0; i < nr_pages; i++) {
		struct folio *folio;

		if (list_empty(&cn->balloon_pages))
			break;
		folio = list_first_entry(&cn->balloon_pages, struct folio, lru);
		list_del(&folio->lru);
		cn->nr_balloon--;
		folio_put(folio);
		deflated++;
		if (!(i & 0x3ff))
			cond_resched();
	}

	mutex_unlock(&cn->balloon_mutex);
	put_cram_node(cn);
	return deflated;
}

/*
 * cram_balloon_reconcile() - recompute the balloon target from the current ratio.
 *
 * EFFECTIVE = perceived * min(1, current_ratio/zratio).  balloon = perceived -
 * effective.  When the achieved ratio drops below the configured zratio the
 * device can hold fewer perceived pages, so we hold more in the balloon.  Sets
 * the target and kicks the convergence worker (latest-wins).  Caller holds
 * balloon_mutex.
 */
/* PERCEIVED size: total buddy-managed pages donated to the node (Σ ranges). */
static unsigned long cram_perceived_pages(const struct cram_node *cn)
{
	unsigned long pages = 0;
	unsigned int i;

	for (i = 0; i < cn->nr_ranges; i++)
		pages += range_len(&cn->ranges[i]) >> PAGE_SHIFT;
	return pages;
}

static void cram_balloon_reconcile(struct cram_node *cn)
{
	u64 target = 0;

	if (cn->current_ratio < cn->zratio && cn->zratio)
		target = (u64)cram_perceived_pages(cn) *
			 (cn->zratio - cn->current_ratio) / cn->zratio;
	WRITE_ONCE(cn->target_balloon, target);
	/*
	 * The convergence worker drives direct reclaim and can run for a long
	 * time, so keep it off the shared system_wq where it could stall short
	 * work items.
	 */
	queue_work(system_long_wq, &cn->wmark_work);
}

/*
 * cram_wmark_work_fn() - balloon convergence worker.
 *
 * Drives nr_balloon toward target_balloon, re-reading the target each iteration
 * so rapid set_compression_ratio() updates coalesce to the LATEST snapshot (one
 * worker, one target, so no thrash and it can't oscillate).  Inflating drives
 * direct reclaim (resident CRAM folios written back to physical swap).  When the
 * node can free no more it stops (best-effort).  On finish it drops the
 * block_alloc convergence block, and only that block, never the unregister
 * teardown fence.
 */
static void cram_wmark_work_fn(struct work_struct *work)
{
	struct cram_node *cn = container_of(work, struct cram_node, wmark_work);
	int nid = cn->nid;

	for (;;) {
		unsigned long target = READ_ONCE(cn->target_balloon);
		unsigned long cur = READ_ONCE(cn->nr_balloon);

		if (fatal_signal_pending(current))
			break;
		if (cur < target) {
			if (!cram_balloon_inflate(nid,
						  min(CRAM_WMARK_CHUNK, target - cur)))
				break;	/* node can free no more; best-effort */
		} else if (cur > target) {
			cram_balloon_deflate(nid, min(CRAM_WMARK_CHUNK, cur - target));
		} else {
			break;		/* converged */
		}
		cond_resched();
	}

	/* Convergence done: drop only our own block (not the teardown fence). */
	WRITE_ONCE(cn->converge_block, false);
}

/**
 * cram_register() - donate physical region(s) to CRAM as a private node
 * @nid:         target NUMA node
 * @ranges:      perceived physical regions to donate (struct range, end-inclusive)
 * @n:           number of ranges
 * @zratio:      hw compression ratio, per-mille N:1 (3:1 => 3000, 1:1 => 1000)
 * @ops:         driver callbacks.  ops.owner (= THIS_MODULE) pins the module for
 *               the node's lifetime.  Stored by value.
 * @driver_data: the driver's per-node object, the ops callback cookie and np
 *               owner.  Opaque to CRAM (keyed operations use @nid).
 *
 * CRAM owns the node lifecycle.  It hotplugs each range as an N_MEMORY_PRIVATE
 * node (caps = CRAM_NP_CAPS, backed by &cn->np), onlines movable, and starts
 * kswapd for LRU aging / writeback.  The driver does no hotplug of its own.
 *
 * Return: 0 on success.  -errno on failure (nothing left onlined on failure).
 */
int cram_register(int nid, const struct range *ranges, unsigned int n,
		  u32 zratio, struct cram_ops ops, void *driver_data)
{
	struct cram_node *cn;
	unsigned int i, added = 0;
	int ret;

	if (!cram_valid_nid(nid) || !ranges || !n)
		return -EINVAL;

	/* Pin the driver module for the node's lifetime (NULL = built-in). */
	if (!try_module_get(ops.owner))
		return -EBUSY;

	cn = kzalloc(sizeof(*cn), GFP_KERNEL);
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
	cn->zratio = zratio;
	cn->current_ratio = zratio;	/* assume configured ratio until reported */
	cn->alloc_allowed = true;	/* demotions permitted until the driver revokes */
	cn->ops = ops;
	cn->driver_data = driver_data;
	cn->nid = nid;
	cn->np.owner = driver_data;
	cn->np.caps = CRAM_NP_CAPS;
	refcount_set(&cn->refcount, 1);
	INIT_WORK(&cn->wmark_work, cram_wmark_work_fn);
	INIT_WORK(&cn->promote_work, cram_promote_work_fn);
	spin_lock_init(&cn->promote_lock);
	INIT_LIST_HEAD(&cn->balloon_pages);
	mutex_init(&cn->balloon_mutex);

	mutex_lock(&cram_mutex);

	/* Accretion / re-register onto an existing node is not yet supported. */
	if (rcu_access_pointer(cram_nodes[nid])) {
		mutex_unlock(&cram_mutex);
		ret = -EBUSY;
		goto err_ranges;
	}

	/*
	 * CRAM owns the node.  Hotplug each range as a private node (the first
	 * call node_private_register()s &cn->np) and online it movable.  On a
	 * failure roll back the ranges already added (fresh/empty, so the atomic
	 * ranges-offline succeeds).
	 */
	for (i = 0; i < n; i++) {
		ret = add_private_memory_driver_managed(nid, ranges[i].start,
				range_len(&ranges[i]), "System RAM (cram)",
				MHP_MERGE_RESOURCE, MMOP_ONLINE_MOVABLE, &cn->np);
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
	 * node_is_cram() / cram_pick_node() observe the node, cram_nodes[nid] is
	 * already valid.  Teardown clears the mask first, the mirror image.
	 */
	static_branch_inc(&cram_enabled);
	rcu_assign_pointer(cram_nodes[nid], cn);
	node_set(nid, cram_node_mask);

	/* Start kswapd on the private node for LRU aging and writeback */
	kswapd_run(nid);

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
 * this is the node's teardown (S1 supports whole-node removal only).  Drop the
 * static key and the module ref.  cn->np outlives the removal, so cn is freed
 * afterwards.
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
	cram_balloon_deflate(nid, ULONG_MAX);

	/*
	 * Block new demotions from picking this node before offline, so a reclaim
	 * pass that already cleared cram_pick_node() cannot land a folio on the node
	 * we are about to remove.  Resident CRAM folios (anon and clean file) are
	 * ordinary movable LRU folios in place, so offline migrates/drops them via
	 * the generic path with no bespoke drop pass.  Left set on the -EBUSY
	 * rollback, like the deflated balloon above, so the driver retries teardown.
	 */
	cram_set_migration_blocked(nid, true);

	/*
	 * Atomic all-or-nothing.  A failure rolls back and removes nothing, so the
	 * node is left intact (still online + published) for the driver to retry.
	 * The range is isolated during offline, so demotions can't land on it.
	 */
	ret = offline_and_remove_memory_ranges(cn->ranges, cn->nr_ranges);
	if (ret) {
		mutex_unlock(&cram_mutex);
		return ret;		/* -EBUSY: node intact, ref retained */
	}

	/* Success: tear down node bookkeeping. */
	owner = cn->ops.owner;
	node_clear(nid, cram_node_mask);
	rcu_assign_pointer(cram_nodes[nid], NULL);
	mutex_unlock(&cram_mutex);

	kswapd_stop(nid);

	/* Drain RCU readers / refcount holders before freeing cn. */
	synchronize_rcu();
	while (!refcount_dec_if_one(&cn->refcount))
		cond_resched();

	/*
	 * The node is now unpublished and ref-drained.  No reporter can queue a
	 * promotion and no ratio update can requeue the balloon worker.  Cancel
	 * both AFTER the drain.  A cancel before it could race a requeue from an
	 * in-flight ref holder (e.g. cram_set_compression_ratio -> reconcile ->
	 * queue_work) and later run the worker on the freed cn.
	 */
	cancel_work_sync(&cn->wmark_work);
	cancel_work_sync(&cn->promote_work);

	static_branch_dec(&cram_enabled);
	kfree(cn->ranges);
	kfree(cn);
	module_put(owner);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_unregister);

/**
 * cram_set_compression_ratio() - driver reports the node's achieved compression
 * @nid: CRAM node
 * @ratio: achieved ratio, per-mille N:1 (same format as the registered zratio)
 * @block_alloc: if true, block new demotions for the duration of the adjustment
 *               (danger mode: the device is over-committed NOW)
 *
 * Resizes the balloon so EFFECTIVE capacity tracks the device's real physical
 * backing as the achieved ratio drifts from the configured zratio.  O(1):
 * publish the target and kick the convergence worker (latest-wins).  Convergence
 * (which may reclaim) is async.  Returns 0 on accept.
 */
int cram_set_compression_ratio(int nid, u32 ratio, bool block_alloc)
{
	struct cram_node *cn;

	if (ratio < 1000)		/* < 1:1 is nonsensical */
		return -EINVAL;

	cn = get_cram_node(nid);
	if (!cn)
		return -ENODEV;

	mutex_lock(&cn->balloon_mutex);
	cn->current_ratio = ratio;
	if (block_alloc)
		WRITE_ONCE(cn->converge_block, true);
	cram_balloon_reconcile(cn);	/* sets target + kicks worker */
	mutex_unlock(&cn->balloon_mutex);

	put_cram_node(cn);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_set_compression_ratio);

/**
 * cram_allow_allocation() - sticky enable/disable of demotions onto a CRAM node
 * @nid: CRAM node
 * @allow: true re-permits demotions, false revokes them
 *
 * The emergency off switch, distinct from set_compression_ratio()'s transient
 * block_alloc.  alloc_allowed is STICKY and stays in effect until the driver
 * clears it.  Disabling does not touch the balloon or in-place folios.  It only
 * stops cram_pick_node() from selecting the node, so reclaim spills elsewhere
 * (other CRAM nodes / swap).  Returns 0 on accept.
 */
int cram_allow_allocation(int nid, bool allow)
{
	struct cram_node *cn;

	cn = get_cram_node(nid);
	if (!cn)
		return -ENODEV;

	WRITE_ONCE(cn->alloc_allowed, allow);
	put_cram_node(cn);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_allow_allocation);

/**
 * cram_report_hot_pages() - the device nominates resident pages for promotion
 * @nid: CRAM node
 * @pfns: flat array of hot pfns (the driver trims, CRAM does not re-threshold)
 * @n: number of pfns
 *
 * CRAM reads are in-place and never fault, so the kernel has no visibility into
 * how hot a resident CRAM page is.  Only the device, which decompresses every
 * access, does.  This is how the device hands that hotness back so CRAM can
 * proactively promote the hottest folios to DRAM (the inverse of demotion).
 *
 * Advisory and asynchronous.  The pfns are queued and a worker resolves and
 * migrates them, whole-folio, best-effort, subject to DRAM headroom and the
 * NUMA-balancing promotion rate limit.  Ring overflow, rate-limited, and stale
 * nominations are silently dropped and the device re-reports later.  Returns 0.
 */
int cram_report_hot_pages(int nid, const unsigned long *pfns, unsigned int n)
{
	struct cram_node *cn;
	unsigned int i;

	cn = get_cram_node(nid);
	if (!cn)
		return -ENODEV;

	spin_lock(&cn->promote_lock);
	for (i = 0; i < n && cn->promote_nr < CRAM_PROMOTE_RING; i++)
		cn->promote_ring[cn->promote_nr++] = pfns[i];
	spin_unlock(&cn->promote_lock);

	queue_work(system_long_wq, &cn->promote_work);
	put_cram_node(cn);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_report_hot_pages);

static int cram_nodes_show(struct seq_file *m, void *v)
{
	int nid;

	seq_puts(m, "node zratio current_ratio perceived balloon target blocked allowed free present\n");
	for_each_node_mask(nid, cram_node_mask) {
		struct cram_node *cn = get_cram_node(nid);

		if (!cn)
			continue;
		seq_printf(m, "%d %u %u %lu %lu %lu %d %d %lu %lu\n",
			   nid,
			   cn->zratio,
			   READ_ONCE(cn->current_ratio),
			   cram_perceived_pages(cn),
			   READ_ONCE(cn->nr_balloon),
			   READ_ONCE(cn->target_balloon),
			   READ_ONCE(cn->migration_blocked) ||
				   READ_ONCE(cn->converge_block),
			   READ_ONCE(cn->alloc_allowed),
			   sum_zone_node_page_state(nid, NR_FREE_PAGES),
			   node_present_pages(nid));
		put_cram_node(cn);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cram_nodes);

static int __init cram_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("cram", NULL);
	if (!IS_ERR(dir)) {
		debugfs_create_ulong("demote_count", 0444, dir,
				     (unsigned long *)&cram_cnt_demote);
		debugfs_create_ulong("promote_count", 0444, dir,
				     (unsigned long *)&cram_cnt_promote);
		debugfs_create_ulong("promote_fail", 0444, dir,
				     (unsigned long *)&cram_cnt_promote_fail);
		debugfs_create_file("nodes", 0444, dir, NULL, &cram_nodes_fops);
	}
	return 0;
}
late_initcall(cram_debugfs_init);
