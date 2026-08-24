// SPDX-License-Identifier: GPL-2.0
/*
 * mm/cram.c - Compressed RAM / private node memory management
 *
 * Manages folios demoted to private nodes via the standard kernel LRU.  The
 * node joins the memory-tier hierarchy, so reclaim demotes onto it through the
 * generic tiering pass; folios the tier cannot hold are refused by
 * folio_placement_eligible() and carry on to swap.  They map present read-only
 * so reads are zero-copy.  The device decompresses in place.
 * A byte write promotes the folio back to DRAM.  Anon promotes via the COW
 * path in do_wp_page().  File promotes via promote_fenced_folio() from the
 * filemap/memory/mprotect write-fence gates, which is generic - the fence is a
 * node feature, not a CRAM interface.  Clean file folios are re-read
 * from the fs when reclaimed rather than swapped.  The device also nominates
 * hot pages for proactive promotion.  See cram_report_hot_pages().
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
#include <linux/balloon.h>
#include <linux/cram.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/jiffies.h>
#include <linux/jump_label.h>
#include <linux/highmem.h>
#include <linux/memory-tiers.h>
#include <linux/memory_hotplug.h>
#include <linux/node.h>
#include <linux/oom.h>
#include <linux/list.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/mmzone.h>
#include <linux/mutex.h>
#include <linux/nodemask.h>
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

/*
 * Node opt-ins for a CRAM node.  RECLAIM lets kswapd and direct reclaim write
 * resident CRAM folios back to physical swap; DEMOTION puts the node in the
 * tier hierarchy so reclaim can demote onto it in the first place.  Teardown
 * needs no opt-in of its own - a driver vetoes unplug through the memory
 * hotplug notifiers.  Withholding NODE_MEMORY_FEAT_USER_WRITE maps folios
 * read-only so a write must COW-promote off the device (folio_must_cow()).
 */
#define CRAM_NODE_FEATURES	(NODE_MEMORY_FEAT_RECLAIM | \
				 NODE_MEMORY_FEAT_DEMOTION)
/*
 * USER_WRITE is deliberately NOT declared: the device is read-only to
 * userspace, so a write must relocate the folio rather than land in place.
 * That is a service the node withholds, asked of a folio ALREADY here, at
 * every site where a PTE could become writable.
 *
 * What may arrive here in the first place follows from the same withholding,
 * and core mm answers it without knowing what CRAM is: folio_wrfence_eligible()
 * takes anon, which leaves by swap (cram_swap.sh measures it), and page cache
 * only while it is still clean, re-readable and reclaimable -- the one state
 * this tier can afford to evict.
 */

/*
 * Abstract distance for a CRAM node whose provider does not supply one.  Same
 * default dax/kmem uses: far enough below DRAM to make the node a demotion
 * target rather than a peer.
 */
#define CRAM_DEFAULT_ADISTANCE	(MEMTIER_ADISTANCE_DRAM * 5)

static DEFINE_MUTEX(cram_memory_type_lock);
static LIST_HEAD(cram_memory_types);

/**
 * cram_setup_memory_tier() - place the node in the demotion hierarchy
 * @nid: the node
 * @adistance: provider's abstract distance, or 0 for the default
 *
 * Reclaim reaches the node through next_demotion_node(), which walks the tier
 * hierarchy, so a CRAM node has to sit in a tier below DRAM.  A node with no
 * memtype defaults to default_dram_type, i.e. a DRAM peer, and would never be
 * demoted to.
 *
 * init_node_memory_type() only assigns when the node has no type yet, so an
 * adistance already established by the platform (HMAT) or by the driver wins
 * and this call just takes a reference.
 */
static int cram_setup_memory_tier(int nid, int adistance)
{
	struct memory_dev_type *mtype;

	if (!adistance)
		adistance = CRAM_DEFAULT_ADISTANCE;

	guard(mutex)(&cram_memory_type_lock);
	mtype = mt_find_alloc_memory_type(adistance, &cram_memory_types);
	if (IS_ERR(mtype))
		return PTR_ERR(mtype);

	init_node_memory_type(nid, mtype);
	return 0;
}

static void cram_teardown_memory_tier(int nid)
{
	guard(mutex)(&cram_memory_type_lock);
	clear_node_memory_type(nid, NULL);
	mt_put_memory_types(&cram_memory_types);
}

/* Pages per proactive-reclaim chunk in the watermark-low loop */
#define CRAM_WMARK_CHUNK	(SZ_8M / PAGE_SIZE)

/* Pages handed to the driver's trim callback per call, and -EAGAIN retry cap. */
#define CRAM_TRIM_BATCH		512
#define CRAM_DEFLATE_RETRIES	8
#define CRAM_TRIM_RETRIES	8

/*
 * Consecutive convergence rounds that free nothing at all before the worker
 * escalates to an OOM kill.  One failed reservation is not evidence of a dead
 * end -- a round can reclaim or evacuate and still lose the race to reserve
 * what it freed -- so require the node to be genuinely stuck.
 */
#define CRAM_CONVERGE_STALL	3

/*
 * Bounded ring of device-nominated hot pfns awaiting proactive promotion.
 * Overflow is dropped (the device re-reports later); sizing it generously keeps
 * a single report_hot_pages() burst from spilling.
 */
#define CRAM_PROMOTE_RING	1024

/* Observability counters, exposed via debugfs */
static atomic_long_t cram_cnt_promote = ATOMIC_LONG_INIT(0);
static atomic_long_t cram_cnt_promote_fail = ATOMIC_LONG_INIT(0);

struct cram_node {
	bool			alloc_allowed;	/* sticky: driver permits demotions */
	bool			migration_blocked; /* external block: teardown / [TEST] */
	bool			converge_block;	/* block_alloc convergence (worker) */
	bool			zone_withdrawn;	/* we hold ZONE_NO_ALLOC */
	bool			test_block;	/* [TEST] debugfs withdrawal */
	int			nid;
	u32			zratio;		/* hw compression ratio, per-mille N:1 */
	u32			current_ratio;	/* achieved ratio, per-mille */
	struct range		*ranges;	/* donated regions */
	unsigned int		nr_ranges;
	refcount_t		refcount;
	struct cram_ops		ops;		/* driver callbacks (by value) */
	void			*driver_data;	/* opaque, passed to ops callbacks */
	struct work_struct	wmark_work;	/* balloon convergence worker */
	struct balloon_dev_info	balloon;	/* reserved pages, held out of the buddy */
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
 * node_states.  CRAM's own walks iterate it directly (for_each_node_mask)
 * instead of filtering the wider node-private set.  The array stays the
 * authority for a node's control state (refcount, alloc_allowed, ...); the
 * mask just answers "is it CRAM", for the debugfs verbs that take an nid.
 */
static nodemask_t cram_node_mask __read_mostly;

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

/*
 * Publish "not accepting" as zone state rather than answering a callback.
 *
 * Three internal reasons -- the driver revoked permission, teardown is in
 * flight, a convergence is running -- and one bit to carry them, because
 * zone_set_no_alloc() claims the zone rather than counting.  Sync on the
 * aggregate edge; callers hold balloon_mutex.
 *
 * The allocator, reclaim, compaction and the OOM constraint all read the flag
 * for themselves.  Demotion in particular allocates on its target, so a
 * withdrawn zone refuses it with no CRAM-specific hook anywhere in mm.
 */
static void cram_release_zone_withdrawal(struct cram_node *cn)
{
	struct zone *zone = &NODE_DATA(cn->nid)->node_zones[ZONE_MOVABLE];

	/*
	 * Quiesce the worker before offline, not merely aim it at zero.  Offline
	 * migrates the balloon's pages; cram_balloon_migratepage() trylocks
	 * balloon_mutex and answers -EAGAIN when it cannot take it, and offline
	 * retries movable pages without bound.  A worker inside
	 * cram_balloon_inflate() holds that mutex across the whole ACR
	 * escalation ladder, so the two make no progress against each other.
	 *
	 * Safe here because the target is already 0: a worker requeued between
	 * this and the cancel after the ref drain re-reads the target and exits
	 * rather than inflating.  That later cancel stays; it guards a different
	 * race (a requeue from an in-flight ref holder running the worker on a
	 * freed cn) and has to come after the drain.
	 */
	cancel_work_sync(&cn->wmark_work);

	/*
	 * A cram_node is freed at unregister; the zone is not.  Teardown leaves
	 * migration_blocked set on purpose so a rolled-back unregister stays
	 * blocked, which means the withdrawal would outlive the node that owns
	 * it -- and the next node to register starts with zone_withdrawn false,
	 * so nothing would ever clear it.  Drop the claim with the node.
	 */
	if (cn->zone_withdrawn) {
		zone_clear_no_alloc(zone);
		cn->zone_withdrawn = false;
	}
}

static void cram_sync_zone_withdrawal(struct cram_node *cn)
{
	struct zone *zone = &NODE_DATA(cn->nid)->node_zones[ZONE_MOVABLE];
	bool want = !READ_ONCE(cn->alloc_allowed) ||
		    READ_ONCE(cn->migration_blocked) ||
		    READ_ONCE(cn->converge_block) ||
		    READ_ONCE(cn->test_block);

	if (want == cn->zone_withdrawn)
		return;
	if (want) {
		if (!zone_set_no_alloc(zone))
			cn->zone_withdrawn = true;
	} else {
		zone_clear_no_alloc(zone);
		cn->zone_withdrawn = false;
	}
}

static void cram_set_migration_blocked(int nid, bool blocked)
{
	struct cram_node *cn = get_cram_node(nid);

	if (!cn)
		return;

	/*
	 * Take a reference rather than staying under RCU: publishing the
	 * withdrawal needs balloon_mutex, which serialises the single claim
	 * against the convergence worker.  No caller holds it.
	 */
	mutex_lock(&cn->balloon_mutex);
	WRITE_ONCE(cn->migration_blocked, blocked);
	cram_sync_zone_withdrawal(cn);
	mutex_unlock(&cn->balloon_mutex);
	put_cram_node(cn);
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

/*
 * Anonymous CRAM folios promote out of the tier via the core COW path.  A write
 * fault reaches wp_page_copy() because wp_can_reuse_anon_folio()
 * refuses CRAM folios (like KSM), COWing into a fresh anon DRAM folio.  The
 * device also nominates hot resident folios for proactive promotion, migrated
 * in place below (cram_promote_folio_to / cram_report_hot_pages).
 */

/*
 * cram_promote_folio_to() - migrate one resident CRAM folio to DRAM in place.
 *
 * The read-side inverse of demotion, driven by device-reported hotness rather
 * than by a write, so it is best-effort: a transient failure is dropped and the
 * device re-reports.  The folio stays readable throughout, since rmap repoints
 * the PTEs and nothing writes it.
 *
 * Consumes the caller's reference on @folio.  Counted in debugfs rather than in
 * pgpromote_fence: that counter is the write fence relieving itself, and this
 * is a capacity decision the device made.
 */
static bool cram_promote_folio_to(struct folio *folio, int dst_nid)
{
	if (migrate_folio_to_node(folio, dst_nid)) {
		atomic_long_inc(&cram_cnt_promote_fail);
		return false;
	}
	atomic_long_inc(&cram_cnt_promote);
	return true;
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

	dst = n ? nearest_public_node(cn->nid) : NUMA_NO_NODE;
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
		 * Must still be on this node (not freed, not already promoted).
		 * cram_promote_folio_to() consumes the ref.  The skip-path drops it.
		 *
		 * The promotion budget is checked HERE, not in the helper: it
		 * bounds how eagerly we act on device-reported hotness, which is
		 * opportunistic.  Evacuating a node whose backing has gone is
		 * not, and must not be throttled by it.
		 */
		if (folio_nid(folio) == cn->nid &&
		    !numa_promotion_rate_limited(dst, folio_nr_pages(folio)))
			cram_promote_folio_to(folio, dst);
		else
			folio_put(folio);

		if (need_resched())
			cond_resched();
	}
	kfree(batch);
}

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

/*
 * Hand a batch of just-reserved pages to the balloon, after dropping their
 * device backing.  Batched because balloon_page_list_enqueue() holds a lock
 * with interrupts off for the whole list, and a convergence can reserve
 * hundreds of thousands of pages at once.
 */
static void cram_balloon_take(struct cram_node *cn, struct list_head *batch,
			      unsigned long *pfns, int *res, unsigned int n)
{
	if (pfns && res)
		cram_trim_batch(cn, pfns, res, n);
	cn->nr_balloon += balloon_page_list_enqueue(&cn->balloon, batch);
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
	int res;

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
	cram_trim_batch(cn, &pfn, &res, 1);

	mutex_unlock(&cn->balloon_mutex);
	return 0;
}

/*
 * One pageblock's worth of pages, acquired whole.
 *
 * alloc_contig_range() is the acquisition primitive rather than the allocator
 * because a converging node has usually withdrawn its own zone with
 * zone_set_no_alloc(), and an allocation would be refused by the flag its owner
 * set.  ACR is PFN-addressed -- it never walks a zonelist, so zone_allows_alloc()
 * never sees it -- which is the same reason unplug keeps working on a withdrawn
 * zone.  It also does both halves of the old drain-then-allocate pair in one
 * call: residents in the range are migrated out, free pages are taken as they
 * are.
 *
 * Pageblock rather than memory block.  The unit only has to be something ACR can
 * isolate, and a pageblock is the natural one; a memory block would quantise
 * capacity to 128MB-2GB for no gain.
 *
 * Return: pages added to @pages, or 0 if the range could not be cleared.
 */
static unsigned long cram_acquire_block(unsigned long start_pfn,
					unsigned long nr, acr_flags_t acr,
					struct list_head *pages)
{
	unsigned long pfn;

	/*
	 * Cheap skip for a block we already hold.  Wrong only after a
	 * reservation has been migrated within the node, and then merely
	 * wasteful: ACR moves the balloon pages aside and we take the block
	 * anyway.
	 */
	if (PageOffline(pfn_to_page(start_pfn)))
		return 0;

	if (alloc_contig_range(start_pfn, start_pfn + nr, acr, GFP_KERNEL))
		return 0;

	for (pfn = start_pfn; pfn < start_pfn + nr; pfn++)
		list_add(&pfn_to_page(pfn)->lru, pages);

	return nr;
}

/**
 * cram_balloon_inflate() - reserve pages on the CRAM node (reduce usable capacity)
 * @nid: CRAM private node
 * @nr_pages: pages to reserve
 *
 * Takes pageblocks off the node and holds them, so demotion cannot use that
 * capacity.  Clearing a block migrates its residents, which on an overcommitted
 * node means driving them out to physical swap (the node has RECLAIM) -- that is
 * how the reservation is paid for.  Reserved pages go to the driver's trim
 * callback, or are zeroed, so the device can reclaim their physical backing.
 *
 * Walks the node's own donated ranges, so it is bounded by the node and never
 * touches anyone else's memory.  A block that will not clear is skipped rather
 * than retried; the caller's convergence loop comes back round.
 *
 * Interruptible.  A large inflate can spend a long time migrating and swapping,
 * so it bails on a fatal signal and stays killable.
 *
 * Return: number of pages actually inflated (may be less than requested when
 * the node cannot free any more).
 */
static unsigned long cram_balloon_inflate(int nid, unsigned long nr_pages,
					  acr_flags_t acr)
{
	const unsigned long blk = pageblock_nr_pages;
	unsigned long inflated = 0;
	unsigned long *trim_pfns;
	unsigned int nbatch = 0;
	struct page *page, *next;
	unsigned int r;
	int *trim_res;
	struct cram_node *cn;
	LIST_HEAD(new_pages);
	LIST_HEAD(batch);

	cn = get_cram_node(nid);
	if (!cn)
		return 0;

	/* Trim scratch: on OOM, reservations still proceed (just untrimmed). */
	trim_pfns = kmalloc_array(CRAM_TRIM_BATCH, sizeof(*trim_pfns), GFP_KERNEL);
	trim_res = kmalloc_array(CRAM_TRIM_BATCH, sizeof(*trim_res), GFP_KERNEL);

	mutex_lock(&cn->balloon_mutex);

	for (r = 0; r < cn->nr_ranges && inflated < nr_pages; r++) {
		unsigned long pfn = PFN_UP(cn->ranges[r].start);
		unsigned long end = PFN_DOWN(cn->ranges[r].end + 1);

		pfn = ALIGN(pfn, blk);
		for (; pfn + blk <= end && inflated < nr_pages; pfn += blk) {
			if (fatal_signal_pending(current))
				goto done;
			inflated += cram_acquire_block(pfn, blk, acr, &new_pages);
			cond_resched();
		}
	}
done:

	/* Drop device backing, then hand each batch to the balloon. */
	list_for_each_entry_safe(page, next, &new_pages, lru) {
		list_move_tail(&page->lru, &batch);
		if (trim_pfns && trim_res)
			trim_pfns[nbatch] = page_to_pfn(page);
		if (++nbatch == CRAM_TRIM_BATCH) {
			cram_balloon_take(cn, &batch, trim_pfns, trim_res, nbatch);
			nbatch = 0;
			cond_resched();
		}
	}
	if (nbatch)
		cram_balloon_take(cn, &batch, trim_pfns, trim_res, nbatch);

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
	unsigned int attempt;

	cn = get_cram_node(nid);
	if (!cn)
		return 0;

	/*
	 * A page isolated for migration is temporarily off the balloon's list,
	 * so a single pass can come up short while the balloon is not actually
	 * empty.  Retry, dropping the mutex in between: migration needs our
	 * callback to make progress, and the callback cannot wait on us.
	 */
	for (attempt = 0; attempt < CRAM_DEFLATE_RETRIES; attempt++) {
		mutex_lock(&cn->balloon_mutex);

		/* Batched: the dequeue holds a lock with interrupts off. */
		while (deflated < nr_pages) {
			struct page *page, *next;
			LIST_HEAD(batch);

			i = balloon_page_list_dequeue(&cn->balloon, &batch,
						      min(nr_pages - deflated,
					    (unsigned long)CRAM_TRIM_BATCH));
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

		if (deflated >= nr_pages || !READ_ONCE(cn->nr_balloon))
			break;
		cond_resched();
	}
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
	unsigned int stalled = 0;
	int nid = cn->nid;

	for (;;) {
		unsigned long target = READ_ONCE(cn->target_balloon);
		unsigned long cur = READ_ONCE(cn->nr_balloon);

		if (fatal_signal_pending(current))
			break;
		if (cur < target) {
			unsigned long want = min(CRAM_WMARK_CHUNK, target - cur);

			/*
			 * One call, three rungs.  Migration alone is the polite
			 * one; ACR_FLAGS_RECLAIM evicts what will not move, and
			 * ACR_FLAGS_OOM kills a task on the node when even that
			 * frees nothing.  Climb only as far as the situation
			 * warrants, and re-enter at the bottom each round -- a
			 * range that would not clear last time may clear now.
			 */
			if (cram_balloon_inflate(nid, want, ACR_FLAGS_NONE)) {
				stalled = 0;
				goto next;
			}

			/*
			 * Polite migration is spent.  In steady state that is
			 * the right place to stop: the tier is full of memory
			 * that is in use, and the driver can wait.
			 */
			if (!READ_ONCE(cn->converge_block))
				break;

			/*
			 * A convergence is not steady state.  The driver has
			 * already lost the physical backing, so leaving the
			 * node full overcommits the device.
			 */
			if (cram_balloon_inflate(nid, want, ACR_FLAGS_RECLAIM)) {
				stalled = 0;
				goto next;
			}

			/*
			 * Freed nothing.  Give it a few rounds before doing
			 * anything drastic, then accept that every resident
			 * folio is unevictable or pinned, there is no swap, and
			 * migration is refused or the public nodes are full.
			 * Carrying on would leave resident above physical,
			 * which is silent data loss, so kill something using
			 * the node instead.
			 */
			if (++stalled < CRAM_CONVERGE_STALL)
				goto next;
			if (!cram_balloon_inflate(nid, want,
						  ACR_FLAGS_RECLAIM | ACR_FLAGS_OOM))
				break;
			stalled = 0;
		} else if (cur > target) {
			cram_balloon_deflate(nid, min(CRAM_WMARK_CHUNK, cur - target));
		} else {
			break;		/* converged */
		}
next:
		cond_resched();
	}

	/* Convergence done: drop only our own block (not the teardown fence). */
	WRITE_ONCE(cn->converge_block, false);
	cram_sync_zone_withdrawal(cn);
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
 * CRAM owns the node lifecycle.  It hotplugs each range as a private node
 * (features = CRAM_NODE_FEATURES) and onlines it movable.  The driver does no
 * hotplug of its own.  kswapd and kcompactd are started by the hotplug path
 * itself, gated on N_MEMORY_RECLAIM, which CRAM_NODE_FEATURES carries.
 *
 * Return: 0 on success.  -errno on failure (nothing left onlined on failure).
 */
int cram_register(int nid, const struct range *ranges, unsigned int n,
		  u32 zratio, int adistance, struct cram_ops ops,
		  void *driver_data)
{
	struct cram_node *cn;
	unsigned int i, added = 0;
	int ret;

	if (!cram_valid_nid(nid) || !ranges || !n)
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
	cn->zratio = zratio;
	cn->current_ratio = zratio;	/* assume configured ratio until reported */
	cn->alloc_allowed = true;	/* demotions permitted until the driver revokes */
	cn->ops = ops;
	cn->driver_data = driver_data;
	cn->nid = nid;
	refcount_set(&cn->refcount, 1);
	INIT_WORK(&cn->wmark_work, cram_wmark_work_fn);
	INIT_WORK(&cn->promote_work, cram_promote_work_fn);
	spin_lock_init(&cn->promote_lock);
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

	ret = cram_setup_memory_tier(nid, adistance);
	if (ret) {
		mutex_unlock(&cram_mutex);
		goto err_ranges;
	}

	/*
	 * CRAM owns the node.  Hotplug each range as a private node (the
	 * features exclude NODE_MEMORY_FEAT_PUBLIC and
	 * NODE_MEMORY_FEAT_USER_WRITE, so it is private and write-fenced) and
	 * online it movable.  On a failure roll back the ranges already added
	 * (fresh/empty, so the atomic ranges-offline succeeds).
	 */
	for (i = 0; i < n; i++) {
		ret = __add_memory_driver_managed(nid, ranges[i].start,
						  range_len(&ranges[i]),
						  "System RAM (cram)",
				MHP_MERGE_RESOURCE, MMOP_ONLINE_MOVABLE,
				CRAM_NODE_FEATURES);
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
 * this is the node's teardown (S1 supports whole-node removal only).  Drop the
 * static key and the module ref.  cn outlives the removal, so cn is freed
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
	 * pass that already withdrew the zone cannot land a folio on it
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
	cram_teardown_memory_tier(nid);
	owner = cn->ops.owner;
	node_clear(nid, cram_node_mask);
	rcu_assign_pointer(cram_nodes[nid], NULL);
	mutex_unlock(&cram_mutex);

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

	cram_release_zone_withdrawal(cn);

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
	cram_sync_zone_withdrawal(cn);
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
 * withdraws the zone, so reclaim spills elsewhere
 * (other CRAM nodes / swap).  Returns 0 on accept.
 */
int cram_allow_allocation(int nid, bool allow)
{
	struct cram_node *cn;

	cn = get_cram_node(nid);
	if (!cn)
		return -ENODEV;

	mutex_lock(&cn->balloon_mutex);
	WRITE_ONCE(cn->alloc_allowed, allow);
	cram_sync_zone_withdrawal(cn);
	mutex_unlock(&cn->balloon_mutex);
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

/*
 * Exact free count for the introspection row.  sum_zone_node_page_state() reads
 * the per-cpu-batched counters, which drift by a few pages between samples --
 * enough for a test comparing two reads to see a "purge" that never happened.
 * zone_page_state_snapshot() folds the per-cpu diffs in.
 */
static unsigned long cram_node_free_pages(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	unsigned long free = 0;
	int i;

	for (i = 0; i < MAX_NR_ZONES; i++)
		free += zone_page_state_snapshot(&pgdat->node_zones[i],
						 NR_FREE_PAGES);
	return free;
}

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
			   cram_node_free_pages(nid),
			   node_present_pages(nid));
		put_cram_node(cn);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cram_nodes);

/*
 * [TEST] deep low-level balloon/back-pressure introspection.  This is NOT the
 * driver interface.  The real paths are the dax device's cram_compression_ratio
 * (balloon resize) and cram_allow_allocation (admission gate).  Kept only so the
 * writeback-to-swap and fault-latency selftests can drive the balloon / block
 * demotions directly by nid.
 *
 * Commands (one per write):
 *   inflate <nid> <nr_pages>   reserve nr_pages on the node (proactive reclaim)
 *   deflate <nid> <nr_pages>   release nr_pages back
 *   block <nid> / unblock <nid>  stop / resume demotions onto the node
 */
#define CRAM_DEMOTE_BATCH	512

/*
 * [TEST] Place a user range on the CRAM node, standing in for reclaim demotion.
 *
 * Per-folio rather than batched: this is a test verb, and going through
 * migrate_folio_to_node() keeps it on the same exported path the promote side
 * uses instead of open-coding a migration_target_control with the private
 * zonelist selector, which a module cannot reach.
 *
 * Return: pages placed on @nid.
 */
static unsigned long cram_demote_range(int nid, unsigned long start,
				       unsigned long nr)
{
	unsigned long done = 0, off;
	struct page **pages;

	pages = kmalloc_array(CRAM_DEMOTE_BATCH, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return 0;

	for (off = 0; off < nr; off += CRAM_DEMOTE_BATCH) {
		unsigned long batch = min_t(unsigned long, CRAM_DEMOTE_BATCH,
					    nr - off);
		long got, i;

		got = get_user_pages_fast(start + (off << PAGE_SHIFT), batch,
					  0, pages);
		if (got <= 0)
			break;

		for (i = 0; i < got; i++) {
			struct folio *folio = page_folio(pages[i]);
			long np = folio_nr_pages(folio);

			if (folio_nid(folio) == nid) {
				folio_put(folio);
				continue;		/* already placed */
			}
			/* consumes the GUP reference either way */
			if (!migrate_folio_to_node(folio, nid))
				done += np;
		}
		cond_resched();
	}

	kfree(pages);
	return done;
}

static ssize_t cram_control_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	unsigned long n = 0, n2 = 0;
	char kbuf[64], cmd[16];
	int nid, ret;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	ret = sscanf(kbuf, "%15s %d %lu %lu", cmd, &nid, &n, &n2);
	if (ret < 2 || nid < 0 || nid >= MAX_NUMNODES)
		return -EINVAL;

	if (!strcmp(cmd, "inflate")) {
		/*
		 * [TEST] optional third argument selects the escalation rung,
		 * so a test can drive ACR_FLAGS_RECLAIM directly instead of
		 * having to fill DRAM to make migration fail.
		 * 0 = migrate only, 1 = +reclaim, 2 = +reclaim +oom.
		 */
		acr_flags_t acr = ACR_FLAGS_NONE;

		if (ret >= 4 && n2 >= 1)
			acr |= ACR_FLAGS_RECLAIM;
		if (ret >= 4 && n2 >= 2)
			acr |= ACR_FLAGS_OOM;
		cram_balloon_inflate(nid, n, acr);
	} else if (!strcmp(cmd, "deflate")) {
		cram_balloon_deflate(nid, n);
	} else if (!strcmp(cmd, "block")) {
		cram_set_migration_blocked(nid, true);
	} else if (!strcmp(cmd, "unblock")) {
		cram_set_migration_blocked(nid, false);
	} else if (!strcmp(cmd, "demote")) {
		if (ret < 4 || !node_isset(nid, cram_node_mask))
			return -EINVAL;
		cram_demote_range(nid, n, n2);
	} else if (!strcmp(cmd, "noalloc")) {
		/*
		 * [TEST] drive ZONE_NO_ALLOC directly, without needing a
		 * driver that has lost its backing.  CRAM onlines movable, so
		 * the node's ZONE_MOVABLE is the zone the allocator sees.
		 */
		struct cram_node *cn;

		if (ret < 3 || !node_isset(nid, cram_node_mask))
			return -EINVAL;
		/*
		 * Go through the node's own state rather than claiming the
		 * zone directly.  zone_set_no_alloc() has ONE owner by design,
		 * and CRAM is already it -- a second claimant here would take
		 * -EBUSY, or worse, release a withdrawal the convergence
		 * worker was relying on.
		 */
		cn = get_cram_node(nid);
		if (!cn)
			return -EINVAL;
		mutex_lock(&cn->balloon_mutex);
		WRITE_ONCE(cn->test_block, !!n);
		cram_sync_zone_withdrawal(cn);
		mutex_unlock(&cn->balloon_mutex);
		put_cram_node(cn);
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct file_operations cram_control_fops = {
	.write = cram_control_write,
};

static struct dentry *cram_debugfs_dir;

static int __init cram_init(void)
{
	cram_debugfs_dir = debugfs_create_dir("cram", NULL);
	if (!IS_ERR(cram_debugfs_dir)) {
		debugfs_create_ulong("promote_count", 0444, cram_debugfs_dir,
				     (unsigned long *)&cram_cnt_promote);
		debugfs_create_ulong("promote_fail", 0444, cram_debugfs_dir,
				     (unsigned long *)&cram_cnt_promote_fail);
		debugfs_create_file("nodes", 0444, cram_debugfs_dir, NULL,
				    &cram_nodes_fops);
		debugfs_create_file("control", 0200, cram_debugfs_dir, NULL,
				    &cram_control_fops);
	}
	return 0;
}

static void __exit cram_exit(void)
{
	/*
	 * Every registered node holds a reference on its provider's module,
	 * and each of those was taken through cram_register().  A node still
	 * registered here would mean a driver outlived its own unregister, so
	 * there is nothing to tear down but the debugfs tree.
	 */
	WARN_ON_ONCE(!nodes_empty(cram_node_mask));
	debugfs_remove_recursive(cram_debugfs_dir);
}

module_init(cram_init);
module_exit(cram_exit);
MODULE_DESCRIPTION("Compressed-RAM private-node read-only tier");
MODULE_LICENSE("GPL");
