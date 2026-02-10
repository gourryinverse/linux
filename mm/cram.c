// SPDX-License-Identifier: GPL-2.0
/*
 * mm/cram.c - Compressed RAM / private node memory management
 *
 * Copyright 2026 Meta Technologies Inc.
 *   Author: Gregory Price <gourry@gourry.net>
 *
 * Manages folios demoted to N_MEMORY_PRIVATE nodes via the standard kernel
 * LRU.  Folios are aged by kswapd on the private node and reclaimed to swap
 * (demotion is suppressed for private nodes).  Write faults trigger promotion
 * back to regular DRAM via the ops->handle_fault callback.
 *
 * All reclaim/demotion uses the standard vmscan infrastructure. Device pressure
 * is communicated via watermark_boost on the private node's zone.
 */

#include <linux/atomic.h>
#include <linux/cpuset.h>
#include <linux/cram.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/jiffies.h>
#include <linux/highmem.h>
#include <linux/memory-tiers.h>
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
#include <linux/swap.h>

#include "internal.h"

struct cram_node {
	void		*owner;
	bool		purged;		/* node is being torn down */
	unsigned int	pressure;
	refcount_t	refcount;
	cram_flush_cb_t	flush_cb;	/* optional driver flush callback */
	void		*flush_data;	/* opaque data for flush_cb */
};

static struct cram_node *cram_nodes[MAX_NUMNODES];
static DEFINE_MUTEX(cram_mutex);

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

static void cram_zero_folio(struct folio *folio)
{
	unsigned int i, nr = folio_nr_pages(folio);

	if (want_init_on_free())
		return;

	for (i = 0; i < nr; i++)
		clear_highpage(folio_page(folio, i));
}

static bool cram_free_folio_cb(struct folio *folio)
{
	int nid = folio_nid(folio);
	struct cram_node *cn;
	int ret;

	cn = get_cram_node(nid);
	if (!cn)
		goto zero_and_free;

	if (!cn->flush_cb)
		goto zero_and_free_put;

	ret = cn->flush_cb(folio, cn->flush_data);
	put_cram_node(cn);

	switch (ret) {
	case 0:
		/* Flush resolved: return to buddy (already zeroed by device) */
		return false;
	case 1:
		/* Deferred: driver holds a ref, do not free to buddy */
		return true;
	case 2:
	default:
		/* Buffer full or unknown: zero locally, return to buddy */
		goto zero_and_free;
	}

zero_and_free_put:
	put_cram_node(cn);
zero_and_free:
	cram_zero_folio(folio);
	return false;
}

static struct folio *alloc_cram_folio(struct folio *src, unsigned long private)
{
	int nid = (int)private;
	unsigned int order = folio_order(src);
	gfp_t gfp = GFP_PRIVATE | __GFP_KSWAPD_RECLAIM |
		     __GFP_HIGHMEM | __GFP_MOVABLE |
		     __GFP_NOWARN | __GFP_NORETRY;

	/* Stop allocating if backpressure fired mid-batch */
	if (node_private_migration_blocked(nid))
		return NULL;

	if (order)
		gfp |= __GFP_COMP;

	return __folio_alloc_node(gfp, order, nid);
}

static void cram_put_new_folio(struct folio *folio, unsigned long private)
{
	cram_zero_folio(folio);
	folio_put(folio);
}

/*
 * Allocate a DRAM folio for promotion out of a private node.
 *
 * Unlike alloc_migration_target(), this does NOT strip __GFP_RECLAIM for
 * large folios, the generic helper does that because THP allocations are
 * opportunistic, but promotion from a private node is mandatory: the page
 * MUST move to DRAM or the process cannot make forward progress.
 *
 * __GFP_RETRY_MAYFAIL tells the allocator to try hard (multiple reclaim
 * rounds, wait for writeback) before giving up.
 */
static struct folio *alloc_cram_promote_folio(struct folio *src,
					      unsigned long private)
{
	int nid = (int)private;
	unsigned int order = folio_order(src);
	gfp_t gfp = GFP_HIGHUSER_MOVABLE | __GFP_RETRY_MAYFAIL;

	if (order)
		gfp |= __GFP_COMP;

	return __folio_alloc(gfp, order, nid, NULL);
}

static int cram_migrate_to(struct list_head *demote_folios, int to_nid,
			   enum migrate_mode mode,
			   enum migrate_reason reason,
			   unsigned int *nr_succeeded)
{
	struct cram_node *cn;
	unsigned int nr_success = 0;
	int ret = 0;

	cn = get_cram_node(to_nid);
	if (!cn)
		return -ENODEV;

	if (cn->purged) {
		ret = -ENODEV;
		goto out;
	}

	/* Block new demotions at maximum pressure */
	if (READ_ONCE(cn->pressure) >= CRAM_PRESSURE_MAX) {
		ret = -ENOSPC;
		goto out;
	}

	ret = migrate_pages(demote_folios, alloc_cram_folio, cram_put_new_folio,
			    (unsigned long)to_nid, mode, reason,
			    &nr_success);

	/*
	 * migrate_folio_move() calls folio_add_lru() for each migrated
	 * folio, but that only adds the folio to a per-CPU batch, 
	 * PG_lru is not set until the batch is drained.  Drain now so
	 * that cram_fault() can isolate these folios immediately.
	 *
	 * Use lru_add_drain_all() because migrate_pages() may process
	 * folios across CPUs, and the local drain might miss batches
	 * filled on other CPUs.
	 */
	if (nr_success)
		lru_add_drain_all();
out:
	put_cram_node(cn);
	if (nr_succeeded)
		*nr_succeeded = nr_success;
	return ret;
}

static void cram_release_ptl(struct vm_fault *vmf, enum pgtable_level level)
{
	if (level == PGTABLE_LEVEL_PTE)
		pte_unmap_unlock(vmf->pte, vmf->ptl);
	else
		spin_unlock(vmf->ptl);
}

static vm_fault_t cram_fault(struct folio *folio, struct vm_fault *vmf,
			     enum pgtable_level level)
{
	struct folio *f, *f2;
	struct cram_node *cn;
	unsigned int nr_succeeded = 0;
	int nid;
	LIST_HEAD(folios);

	nid = folio_nid(folio);

	cn = get_cram_node(nid);
	if (!cn) {
		cram_release_ptl(vmf, level);
		return 0;
	}

	/*
	 * Isolate from LRU while holding PTL.  This serializes against
	 * other CPUs faulting on the same folio: only one CPU can clear
	 * PG_lru under the PTL, and it proceeds to migration.  Other
	 * CPUs find the folio already isolated and bail out, preventing
	 * the refcount pile-up that causes migrate_pages() to fail with
	 * -EAGAIN.
	 *
	 * No explicit folio_get() is needed: the page table entry holds
	 * a reference (we still hold PTL), and folio_isolate_lru() takes
	 * its own reference.  This matches do_numa_page()'s pattern.
	 *
	 * PG_lru should already be set: cram_migrate_to() drains per-CPU
	 * LRU batches after migration, and the failure path below
	 * drains after putback.
	 */
	if (!folio_isolate_lru(folio)) {
		put_cram_node(cn);
		cram_release_ptl(vmf, level);
		cond_resched();
		return 0;
	}

	/* Folio isolated, release PTL, proceed to migration */
	cram_release_ptl(vmf, level);

	node_stat_mod_folio(folio,
			    NR_ISOLATED_ANON + folio_is_file_lru(folio),
			    folio_nr_pages(folio));
	list_add(&folio->lru, &folios);

	migrate_pages(&folios, alloc_cram_promote_folio, NULL,
		      (unsigned long)numa_node_id(),
		      MIGRATE_SYNC, MR_NUMA_MISPLACED, &nr_succeeded);

	/* Put failed folios back on LRU; retry on next fault */
	list_for_each_entry_safe(f, f2, &folios, lru) {
		list_del(&f->lru);
		node_stat_mod_folio(f,
				    NR_ISOLATED_ANON + folio_is_file_lru(f),
				    -folio_nr_pages(f));
		folio_putback_lru(f);
	}

	/*
	 * If migration failed, folio_putback_lru() batched the folio
	 * into this CPU's per-CPU LRU cache (PG_lru not yet set).
	 * Drain now so the folio is immediately visible on the LRU,
	 * the next fault can then isolate it without an IPI storm
	 * via lru_add_drain_all().
	 *
	 * Return VM_FAULT_RETRY after releasing the fault lock so the
	 * arch handler retries from scratch.  Without this, returning 0
	 * causes a tight livelock: the process immediately re-faults on
	 * the same write-protected entry, alloc fails again, and
	 * VM_FAULT_OOM eventually leaks out through a stale path.
	 * VM_FAULT_RETRY gives the system breathing room to reclaim.
	 */
	if (!nr_succeeded) {
		lru_add_drain();
		cond_resched();
		put_cram_node(cn);
		release_fault_lock(vmf);
		return VM_FAULT_RETRY;
	}

	cond_resched();
	put_cram_node(cn);
	return 0;
}

static void cram_folio_migrate(struct folio *src, struct folio *dst)
{
}

static void cram_reclaim_policy(int nid, struct node_reclaim_policy *policy)
{
	policy->may_swap = true;
	policy->may_writepage = true;
	policy->managed_watermarks = true;
}

static vm_fault_t cram_handle_fault(struct folio *folio, struct vm_fault *vmf,
				    enum pgtable_level level)
{
	return cram_fault(folio, vmf, level);
}

static const struct node_private_ops cram_ops = {
	.handle_fault		= cram_handle_fault,
	.migrate_to		= cram_migrate_to,
	.folio_migrate		= cram_folio_migrate,
	.free_folio		= cram_free_folio_cb,
	.reclaim_policy		= cram_reclaim_policy,
	.flags			= NP_OPS_MIGRATION | NP_OPS_DEMOTION |
				  NP_OPS_NUMA_BALANCING | NP_OPS_PROTECT_WRITE |
				  NP_OPS_RECLAIM,
};

int cram_register_private_node(int nid, void *owner,
			       cram_flush_cb_t flush_cb, void *flush_data)
{
	struct cram_node *cn;
	int ret;

	if (!node_state(nid, N_MEMORY_PRIVATE))
		return -EINVAL;

	mutex_lock(&cram_mutex);

	cn = cram_nodes[nid];
	if (cn) {
		if (cn->owner != owner) {
			mutex_unlock(&cram_mutex);
			return -EBUSY;
		}
		mutex_unlock(&cram_mutex);
		return 0;
	}

	cn = kzalloc(sizeof(*cn), GFP_KERNEL);
	if (!cn) {
		mutex_unlock(&cram_mutex);
		return -ENOMEM;
	}

	cn->owner = owner;
	cn->pressure = 0;
	cn->flush_cb = flush_cb;
	cn->flush_data = flush_data;
	refcount_set(&cn->refcount, 1);

	ret = node_private_set_ops(nid, &cram_ops);
	if (ret) {
		mutex_unlock(&cram_mutex);
		kfree(cn);
		return ret;
	}

	rcu_assign_pointer(cram_nodes[nid], cn);

	/* Start kswapd on the private node for LRU aging and reclaim */
	kswapd_run(nid);

	mutex_unlock(&cram_mutex);

	/* Now that ops->migrate_to is set, refresh demotion targets */
	memory_tier_refresh_demotion();
	return 0;
}
EXPORT_SYMBOL_GPL(cram_register_private_node);

int cram_unregister_private_node(int nid)
{
	struct cram_node *cn;

	if (!cram_valid_nid(nid))
		return -EINVAL;

	mutex_lock(&cram_mutex);

	cn = cram_nodes[nid];
	if (!cn) {
		mutex_unlock(&cram_mutex);
		return -ENODEV;
	}

	kswapd_stop(nid);

	WARN_ON(node_private_clear_ops(nid, &cram_ops));
	rcu_assign_pointer(cram_nodes[nid], NULL);
	mutex_unlock(&cram_mutex);

	/* ops->migrate_to cleared, refresh demotion targets */
	memory_tier_refresh_demotion();

	synchronize_rcu();
	while (!refcount_dec_if_one(&cn->refcount))
		cond_resched();
	kfree(cn);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_unregister_private_node);

int cram_unpurge(int nid)
{
	struct cram_node *cn;

	if (!cram_valid_nid(nid))
		return -EINVAL;

	mutex_lock(&cram_mutex);

	cn = cram_nodes[nid];
	if (!cn) {
		mutex_unlock(&cram_mutex);
		return -ENODEV;
	}

	cn->purged = false;

	mutex_unlock(&cram_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(cram_unpurge);

void cram_set_pressure(int nid, unsigned int pressure)
{
	struct cram_node *cn;
	struct node_private *np;
	struct zone *zone;
	unsigned long managed, boost;

	cn = get_cram_node(nid);
	if (!cn)
		return;

	if (pressure > CRAM_PRESSURE_MAX)
		pressure = CRAM_PRESSURE_MAX;

	WRITE_ONCE(cn->pressure, pressure);

	rcu_read_lock();
	np = rcu_dereference(NODE_DATA(nid)->node_private);
	/* Block demotions only at maximum pressure */
	if (np)
		WRITE_ONCE(np->migration_blocked,
			   pressure >= CRAM_PRESSURE_MAX);
	rcu_read_unlock();

	zone = NULL;
	for (int i = 0; i < MAX_NR_ZONES; i++) {
		struct zone *z = &NODE_DATA(nid)->node_zones[i];

		if (zone_managed_pages(z) > 0) {
			zone = z;
			break;
		}
	}
	if (!zone) {
		put_cram_node(cn);
		return;
	}
	managed = zone_managed_pages(zone);

	/* Boost proportional to pressure. 0:no boost, 1000:full managed */
	boost = (managed * (unsigned long)pressure) / CRAM_PRESSURE_MAX;
	WRITE_ONCE(zone->watermark_boost, boost);

	if (boost) {
		set_bit(ZONE_BOOSTED_WATERMARK, &zone->flags);
		wakeup_kswapd(zone, GFP_KERNEL, 0, ZONE_MOVABLE);
	}

	put_cram_node(cn);
}
EXPORT_SYMBOL_GPL(cram_set_pressure);

void cram_clear_pressure(int nid)
{
	cram_set_pressure(nid, 0);
}
EXPORT_SYMBOL_GPL(cram_clear_pressure);
