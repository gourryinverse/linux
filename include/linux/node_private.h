/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NODE_PRIVATE_H
#define _LINUX_NODE_PRIVATE_H

#include <linux/completion.h>
#include <linux/memremap.h>
#include <linux/migrate_mode.h>
#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>

struct page;
struct vm_area_struct;
struct vm_fault;

/**
 * struct node_private_ops - Callbacks for private node services
 *
 * Services register these callbacks to intercept MM operations that affect
 * their private nodes.
 *
 * Flag bits control which MM subsystems may operate on folios on this node.
 *
 * The pgdat->private pointer is RCU-protected.  Callbacks fall into
 * three categories based on their calling context:
 *
 * Folio-referenced callbacks (RCU released before callback):
 *   The caller holds a reference to a folio on the private node, which
 *   pins the node's memory online and prevents node_private teardown.
 *
 * Refcounted callbacks (RCU released before callback):
 *   The caller has no folio on the private node (e.g., folios are on a
 *   source node being migrated TO this node).  A temporary refcount is
 *   taken on node_private under rcu_read_lock to keep the structure (and
 *   the service module) alive across the callback.  node_private_unregister
 *   waits for all temporary references to drain before returning.
 *
 * Non-folio callbacks (rcu_read_lock held during callback):
 *   No folio reference exists, so rcu_read_lock is held across the
 *   callback to prevent node_private from being freed.
 *   These callbacks MUST NOT sleep.
 *
 * @free_folio: Called when a folio refcount drops to 0
 *   [folio-referenced callback]
 *   Returns: true if handled (skip return to buddy)
 *            false if no op (return to buddy)
 *
 * @folio_split: Notification that a folio on this private node is being split.
 *    [folio-referenced callback]
 *     Called from the folio split path via folio_managed_split_cb().
 *     @folio is the original folio; @new_folio is the newly created folio,
 *     or NULL when called for the final (original) folio after all sub-folios
 *     have been split off.
 *
 * @migrate_to: Migrate folios TO this node.
 *	[refcounted callback]
 *	Returns: 0 on full success, >0 = number of folios that failed to
 *		 migrate, <0 = error.  Matches migrate_pages() semantics.
 *		 @nr_succeeded is set to the number of successfully migrated
 *		 folios (may be NULL if caller doesn't need it).
 *
 * @folio_migrate: Post-migration notification that a folio on this private node
 *    changed physical location (on the same node or a different node).
 *    [folio-referenced callback]
 *     Called from migrate_folio_move() after data has been copied but before
 *     migration entries are replaced with real PTEs.  Both @src and @dst are
 *     locked.  Faults block in migration_entry_wait() until
 *     remove_migration_ptes() runs, so the service can safely update
 *     PFN-based metadata (compression tables, device page tables, DMA
 *     mappings, etc.) before any access through the page tables.
 *
 * @memory_failure: Notification of hardware error on a page on this node.
 *   [folio-referenced callback]
 *   Notification only, kernel always handles the failure.
 *
 * @flags: Operation exclusion flags (NP_OPS_* constants).
 *
 */
struct node_private_ops {
	bool (*free_folio)(struct folio *folio);
	void (*folio_split)(struct folio *folio, struct folio *new_folio);
	int (*migrate_to)(struct list_head *folios, int nid,
				  enum migrate_mode mode,
				  enum migrate_reason reason,
				  unsigned int *nr_succeeded);
	void (*folio_migrate)(struct folio *src, struct folio *dst);
	void (*memory_failure)(struct folio *folio, unsigned long pfn,
			       int mf_flags);
	unsigned long flags;
};

/* Allow user/kernel migration; requires migrate_to and folio_migrate */
#define NP_OPS_MIGRATION		BIT(0)
/* Allow mempolicy-directed allocation and mbind migration to this node */
#define NP_OPS_MEMPOLICY		BIT(1)

/**
 * struct node_private - Per-node container for private nodes
 *
 * This structure is allocated by the driver and passed to node_private_register().
 * The driver owns the memory and must ensure it remains valid until after
 * node_private_unregister() returns with the reference count dropped to 0.
 *
 * @owner: Opaque driver identifier
 * @refcount: Reference count (1 = registered; temporary refs for non-folio
 *		callbacks that may sleep; 0 = fully released)
 * @released: Signaled when refcount drops to 0; unregister waits on this
 * @ops: Service callbacks and exclusion flags (NULL until service registers)
 */
struct node_private {
	void *owner;
	refcount_t refcount;
	struct completion released;
	const struct node_private_ops *ops;
};

#ifdef CONFIG_NUMA

#include <linux/mmzone.h>

/**
 * folio_is_private_node - Check if folio is on a private node
 * @folio: The folio to check
 *
 * Returns true if the folio resides on a private node.
 */
static inline bool folio_is_private_node(struct folio *folio)
{
	return node_is_private(folio_nid(folio));
}

/**
 * page_is_private_node - Check if page is on a private node
 * @page: The page to check
 *
 * Returns true if the page resides on a private node.
 */
static inline bool page_is_private_node(struct page *page)
{
	return node_is_private(page_to_nid(page));
}

static inline bool folio_is_private_managed(struct folio *folio)
{
	return folio_is_zone_device(folio) || folio_is_private_node(folio);
}

static inline bool page_is_private_managed(struct page *page)
{
	return folio_is_private_managed(page_folio(page));
}

static inline const struct node_private_ops *
folio_node_private_ops(struct folio *folio)
{
	const struct node_private_ops *ops;
	struct node_private *np;

	rcu_read_lock();
	np = rcu_dereference(NODE_DATA(folio_nid(folio))->private);
	ops = np ? np->ops : NULL;
	rcu_read_unlock();

	return ops;
}

static inline unsigned long node_private_flags(int nid)
{
	struct node_private *np;
	unsigned long flags;

	rcu_read_lock();
	np = rcu_dereference(NODE_DATA(nid)->private);
	flags = (np && np->ops) ? np->ops->flags : 0;
	rcu_read_unlock();

	return flags;
}

static inline bool folio_private_flags(struct folio *f, unsigned long flag)
{
	return node_private_flags(folio_nid(f)) & flag;
}

static inline bool node_private_has_flag(int nid, unsigned long flag)
{
	return node_private_flags(nid) & flag;
}

static inline bool zone_private_flags(struct zone *z, unsigned long flag)
{
	return node_private_flags(zone_to_nid(z)) & flag;
}

static inline bool zone_private_alloc_allowed(struct zone *zone, gfp_t gfp_mask)
{
	int nid = zone_to_nid(zone);

	if (!node_is_private(nid))
		return true;

	return (gfp_mask & __GFP_PRIVATE);
}

static inline void node_private_split_cb(struct folio *folio,
					 struct folio *new_folio)
{
	const struct node_private_ops *ops = folio_node_private_ops(folio);

	if (ops && ops->folio_split)
		ops->folio_split(folio, new_folio);
}

static inline void folio_managed_split_cb(struct folio *original_folio,
					  struct folio *new_folio)
{
	node_private_split_cb(original_folio, new_folio);
}

#ifdef CONFIG_MEMORY_HOTPLUG
static inline int folio_managed_allows_user_migrate(struct folio *folio)
{
	if (folio_is_zone_device(folio))
		return -ENOENT;
	return node_private_has_flag(folio_nid(folio), NP_OPS_MIGRATION) ?
	       folio_nid(folio) : -ENOENT;
}

/**
 * folio_managed_allows_migrate - Check if a managed folio supports migration
 * @folio: The folio to check
 *
 * Returns true if the folio can be migrated.  For zone_device folios, only
 * device_private and device_coherent support migration.  For private node
 * folios, migration requires NP_OPS_MIGRATION.  Normal folios always
 * return true.
 */
static inline bool folio_managed_allows_migrate(struct folio *folio)
{
	if (folio_is_zone_device(folio))
		return folio_is_device_private(folio) ||
		       folio_is_device_coherent(folio);
	if (folio_is_private_node(folio))
		return folio_private_flags(folio, NP_OPS_MIGRATION);
	return true;
}

/**
 * node_private_migrate_to - Attempt service-specific migration to a private node
 * @folios: list of folios to migrate (may sleep)
 * @nid: target node
 * @mode: migration mode (MIGRATE_ASYNC, MIGRATE_SYNC, etc.)
 * @reason: migration reason (MR_DEMOTION, MR_SYSCALL, etc.)
 * @nr_succeeded: optional output for number of successfully migrated folios
 *
 * If @nid is a private node with a migrate_to callback,
 * invokes the callback and returns the result with migrate_pages()
 * semantics (0 = full success, >0 = failure count, <0 = error).
 * Returns -ENODEV if the node is not private or the service is being
 * torn down.
 *
 * The source folios are on other nodes, so they do not pin the target
 * node's node_private.  A temporary refcount is taken under rcu_read_lock
 * to keep node_private (and the service module) alive across the callback.
 */
static inline int node_private_migrate_to(struct list_head *folios, int nid,
					  enum migrate_mode mode,
					  enum migrate_reason reason,
					  unsigned int *nr_succeeded)
{
	int (*fn)(struct list_head *, int, enum migrate_mode,
		  enum migrate_reason, unsigned int *);
	struct node_private *np;
	int ret;

	rcu_read_lock();
	np = rcu_dereference(NODE_DATA(nid)->private);
	if (!np || !np->ops || !np->ops->migrate_to ||
	    !refcount_inc_not_zero(&np->refcount)) {
		rcu_read_unlock();
		return -ENODEV;
	}
	fn = np->ops->migrate_to;
	rcu_read_unlock();

	ret = fn(folios, nid, mode, reason, nr_succeeded);

	if (refcount_dec_and_test(&np->refcount))
		complete(&np->released);

	return ret;
}

static inline bool node_mpol_eligible(int nid)
{
	bool ret;

	if (!node_is_private(nid))
		return node_state(nid, N_MEMORY);

	rcu_read_lock();
	ret = node_private_has_flag(nid, NP_OPS_MEMPOLICY);
	rcu_read_unlock();
	return ret;
}

static inline bool nodes_private_mpol_allowed(const nodemask_t *nodes)
{
	int nid;
	bool eligible = false;

	for_each_node_mask(nid, *nodes) {
		if (!node_is_private(nid))
			continue;
		if (!node_mpol_eligible(nid))
			return false;
		eligible = true;
	}
	return eligible;
}
#endif /* CONFIG_MEMORY_HOTPLUG */

#else /* !CONFIG_NUMA */

static inline bool folio_is_private_node(struct folio *folio)
{
	return false;
}

static inline bool page_is_private_node(struct page *page)
{
	return false;
}

static inline bool folio_is_private_managed(struct folio *folio)
{
	return folio_is_zone_device(folio);
}

static inline bool page_is_private_managed(struct page *page)
{
	return folio_is_private_managed(page_folio(page));
}

static inline const struct node_private_ops *
folio_node_private_ops(struct folio *folio)
{
	return NULL;
}

static inline unsigned long node_private_flags(int nid)
{
	return 0;
}

static inline bool folio_private_flags(struct folio *f, unsigned long flag)
{
	return false;
}

static inline bool node_private_has_flag(int nid, unsigned long flag)
{
	return false;
}

static inline bool zone_private_flags(struct zone *z, unsigned long flag)
{
	return false;
}

static inline bool zone_private_alloc_allowed(struct zone *zone, gfp_t gfp_mask)
{
	return true;
}

static inline void folio_managed_split_cb(struct folio *original_folio,
					  struct folio *new_folio)
{
}
#endif /* CONFIG_NUMA */

#if defined(CONFIG_NUMA) && defined(CONFIG_MEMORY_HOTPLUG)

int node_private_register(int nid, struct node_private *np);
int node_private_unregister(int nid);
int node_private_set_ops(int nid, const struct node_private_ops *ops);
int node_private_clear_ops(int nid, const struct node_private_ops *ops);

#else /* !CONFIG_NUMA || !CONFIG_MEMORY_HOTPLUG */

static inline int folio_managed_allows_user_migrate(struct folio *folio)
{
	return -ENOENT;
}

static inline bool folio_managed_allows_migrate(struct folio *folio)
{
	if (folio_is_zone_device(folio))
		return folio_is_device_private(folio) ||
		       folio_is_device_coherent(folio);
	return true;
}

static inline int node_private_migrate_to(struct list_head *folios, int nid,
					  enum migrate_mode mode,
					  enum migrate_reason reason,
					  unsigned int *nr_succeeded)
{
	return -ENODEV;
}

static inline bool node_mpol_eligible(int nid)
{
	return false;
}

static inline bool nodes_private_mpol_allowed(const nodemask_t *nodes)
{
	return false;
}

static inline int node_private_register(int nid, struct node_private *np)
{
	return -ENODEV;
}

static inline int node_private_unregister(int nid)
{
	return 0;
}

static inline int node_private_set_ops(int nid,
				       const struct node_private_ops *ops)
{
	return -ENODEV;
}

static inline int node_private_clear_ops(int nid,
					 const struct node_private_ops *ops)
{
	return -ENODEV;
}

#endif /* CONFIG_NUMA && CONFIG_MEMORY_HOTPLUG */

#endif /* _LINUX_NODE_PRIVATE_H */
