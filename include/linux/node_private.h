/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NODE_PRIVATE_H
#define _LINUX_NODE_PRIVATE_H

#include <linux/mm.h>
#include <linux/nodemask.h>

struct page;

/*
 * Per-node service opt-ins (node_private.caps).  A private node is isolated
 * from all general mm services by default; the registering driver sets these
 * to let specific services operate on its node.
 */
#define NODE_PRIVATE_CAP_RECLAIM	(1UL << 0)	/* allow mm reclaim */
#define NODE_PRIVATE_CAP_USER_NUMA	(1UL << 1)	/* allow mempolicy */
#define NODE_PRIVATE_CAP_HOTUNPLUG	(1UL << 2)	/* allow hot-unplug */
#define NODE_PRIVATE_CAP_DEMOTION	(1UL << 3)	/* allow tiering demotion */

/**
 * struct node_private - Per-node container for N_MEMORY_PRIVATE nodes
 *
 * Allocated by the driver and passed to node_private_register().
 * The driver owns the memory and must ensure it remains valid until after
 * node_private_unregister() returns.
 *
 * @owner: Opaque driver identifier
 * @caps: NODE_PRIVATE_CAP_* service opt-ins for the node (zero by default;
 *	  individual capabilities are defined and consumed by later changes)
 */
struct node_private {
	void *owner;
	unsigned long caps;
};

#ifdef CONFIG_NUMA
#include <linux/mmzone.h>

static inline bool folio_is_private_node(struct folio *folio)
{
	return node_state(folio_nid(folio), N_MEMORY_PRIVATE);
}

static inline bool page_is_private_node(struct page *page)
{
	return node_state(page_to_nid(page), N_MEMORY_PRIVATE);
}

static inline bool node_is_private(int nid)
{
	return node_state(nid, N_MEMORY_PRIVATE);
}

/**
 * node_allows_reclaim - may the mm reclaim from this node?
 * @nid: the node to test
 *
 * Only a private node is ever excluded.  Every other node can safely
 * be operated on by reclaim.
 */
static inline bool node_allows_reclaim(int nid)
{
	struct node_private *np;
	bool ret;

	if (!node_state(nid, N_MEMORY_PRIVATE))
		return true;
	rcu_read_lock();
	np = rcu_dereference(NODE_DATA(nid)->node_private);
	ret = np && (np->caps & NODE_PRIVATE_CAP_RECLAIM);
	rcu_read_unlock();
	return ret;
}

/**
 * node_allows_user_numa - may userspace place or migrate memory here?
 * @nid: the node to test
 *
 * Gate all userspace-directed memory operations on a private node.
 *   - mbind()/set_mempolicy()
 *   - move_pages()/migrate_pages()
 *
 * return: true for N_MEMORY and N_MEMORY_PRIVATE with CAP_USER_NUMA.
 *         false for memoryless or opted-out private node.
 */
static inline bool node_allows_user_numa(int nid)
{
	struct node_private *np;
	bool ret;

	if (node_state(nid, N_MEMORY))
		return true;
	if (!node_state(nid, N_MEMORY_PRIVATE))
		return false;
	rcu_read_lock();
	np = rcu_dereference(NODE_DATA(nid)->node_private);
	ret = np && (np->caps & NODE_PRIVATE_CAP_USER_NUMA);
	rcu_read_unlock();
	return ret;
}

/**
 * node_allows_hotunplug - may hot-unplug migrate this node's folios?
 * @nid: the node to test
 *
 * True for normal nodes and private nodes opted into CAP_HOTUNPLUG.
 */
static inline bool node_allows_hotunplug(int nid)
{
	struct node_private *np;
	bool ret;

	if (!node_state(nid, N_MEMORY_PRIVATE))
		return true;
	rcu_read_lock();
	np = rcu_dereference(NODE_DATA(nid)->node_private);
	ret = np && (np->caps & NODE_PRIVATE_CAP_HOTUNPLUG);
	rcu_read_unlock();
	return ret;
}

/**
 * node_allows_demotion - may kernel tiering demote to this node?
 * @nid: the node to test
 *
 * Governs whether a private node participates in the demotion hierarchy.
 * Demotion accumulates pages on the node, so CAP_DEMOTION requires CAP_RECLAIM
 * (enforced at registration) as a safety valve.
 *
 * return: true for normal nodes and private nodes opted into CAP_DEMOTION.
 */
static inline bool node_allows_demotion(int nid)
{
	struct node_private *np;
	bool ret;

	if (!node_state(nid, N_MEMORY_PRIVATE))
		return true;
	rcu_read_lock();
	np = rcu_dereference(NODE_DATA(nid)->node_private);
	ret = np && (np->caps & NODE_PRIVATE_CAP_DEMOTION);
	rcu_read_unlock();
	return ret;
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

static inline bool node_is_private(int nid)
{
	return false;
}

static inline bool node_allows_reclaim(int nid)
{
	return true;
}

static inline bool node_allows_user_numa(int nid)
{
	return true;
}

static inline bool node_allows_hotunplug(int nid)
{
	return true;
}

static inline bool node_allows_demotion(int nid)
{
	return true;
}

#endif /* CONFIG_NUMA */

#if defined(CONFIG_NUMA) && defined(CONFIG_MEMORY_HOTPLUG)

int node_private_register(int nid, struct node_private *np);
int node_private_unregister(int nid);

#else /* !CONFIG_NUMA || !CONFIG_MEMORY_HOTPLUG */

static inline int node_private_register(int nid, struct node_private *np)
{
	return -ENODEV;
}

static inline int node_private_unregister(int nid)
{
	return 0;
}

#endif /* CONFIG_NUMA && CONFIG_MEMORY_HOTPLUG */

#endif /* _LINUX_NODE_PRIVATE_H */
