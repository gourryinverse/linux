/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NODE_PRIVATE_H
#define _LINUX_NODE_PRIVATE_H

#include <linux/completion.h>
#include <linux/memremap.h>
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
 * @flags: Operation exclusion flags (NP_OPS_* constants).
 *
 */
struct node_private_ops {
	bool (*free_folio)(struct folio *folio);
	void (*folio_split)(struct folio *folio, struct folio *new_folio);
	unsigned long flags;
};

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
	if (folio_is_zone_device(original_folio))
		zone_device_private_split_cb(original_folio, new_folio);
	else if (folio_is_private_node(original_folio))
		node_private_split_cb(original_folio, new_folio);
}

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

static inline void folio_managed_split_cb(struct folio *original_folio,
					  struct folio *new_folio)
{
	if (folio_is_zone_device(original_folio))
		zone_device_private_split_cb(original_folio, new_folio);
}
#endif /* CONFIG_NUMA */

#if defined(CONFIG_NUMA) && defined(CONFIG_MEMORY_HOTPLUG)

int node_private_register(int nid, struct node_private *np);
int node_private_unregister(int nid);
int node_private_set_ops(int nid, const struct node_private_ops *ops);
int node_private_clear_ops(int nid, const struct node_private_ops *ops);

#else /* !CONFIG_NUMA || !CONFIG_MEMORY_HOTPLUG */

static inline bool zone_private_alloc_allowed(struct zone *zone, gfp_t gfp_mask)
{
	return true;
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
