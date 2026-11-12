// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for private memory node isolation in the page allocator.
 *
 * The checks run against the live node states and zonelists, under the
 * memory hotplug lock so no zonelist is rebuilt while it is walked. Checks
 * for private nodes, including allocations from one, only fire on a system
 * that has some.
 */
#include <kunit/test.h>
#include <linux/cpuset.h>
#include <linux/gfp.h>
#include <linux/memory_hotplug.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>

#include "../page_alloc.h"

/* Whether every zone in @zl belongs to @nid; counts the zones in @nr. */
static bool zonelist_is_local(struct zonelist *zl, int nid, int *nr)
{
	struct zoneref *z;

	*nr = 0;
	for (z = zl->_zonerefs; zonelist_zone(z); z++) {
		if (zonelist_node_idx(z) != nid)
			return false;
		(*nr)++;
	}
	return true;
}

static void private_node_states_partition(struct kunit *test)
{
	nodemask_t both, either;

	get_online_mems();
	nodes_and(both, node_states[N_MEMORY_COMMON],
		  node_states[N_MEMORY_PRIVATE]);
	nodes_or(either, node_states[N_MEMORY_COMMON],
		 node_states[N_MEMORY_PRIVATE]);
	KUNIT_EXPECT_TRUE(test, nodes_empty(both));
	KUNIT_EXPECT_TRUE(test, nodes_equal(either, node_states[N_MEMORY]));
	put_online_mems();
}

static void private_node_features_valid(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, node_memory_features_valid(0));
	KUNIT_EXPECT_TRUE(test, node_memory_features_valid(NODE_MEMORY_FEAT_ALL));
	/* Common memory carries every feature. */
	KUNIT_EXPECT_FALSE(test,
			   node_memory_features_valid(NODE_MEMORY_FEAT_COMMON));
	KUNIT_EXPECT_FALSE(test,
			   node_memory_features_valid(NODE_MEMORY_FEAT_ALL << 1));
}

static void private_node_fallback_zonelists(struct kunit *test)
{
	struct zoneref *z;
	nodemask_t seen;
	int nid;

	get_online_mems();
	for_each_online_node(nid) {
		struct zonelist *zl =
			&NODE_DATA(nid)->node_zonelists[ZONELIST_FALLBACK];

		/* An allocation that names no node never reaches private memory. */
		nodes_clear(seen);
		for (z = zl->_zonerefs; zonelist_zone(z); z++) {
			KUNIT_EXPECT_TRUE(test, node_state(zonelist_node_idx(z),
							   N_MEMORY_COMMON));
			node_set(zonelist_node_idx(z), seen);
		}
		/* ... but reaches every common node. */
		KUNIT_EXPECT_TRUE(test,
				  nodes_equal(seen, node_states[N_MEMORY_COMMON]));
	}
	put_online_mems();
}

static void private_node_local_zonelists(struct kunit *test)
{
	int nid, nr;

	get_online_mems();
	for_each_online_node(nid) {
		struct zonelist *thisnode =
			&NODE_DATA(nid)->node_zonelists[ZONELIST_NOFALLBACK];
		struct zonelist *private =
			&NODE_DATA(nid)->node_zonelists[ZONELIST_PRIVATE];

		/* __GFP_THISNODE reaches a common node's own zones only. */
		KUNIT_EXPECT_TRUE(test, zonelist_is_local(thisnode, nid, &nr));
		if (node_state(nid, N_MEMORY_COMMON))
			KUNIT_EXPECT_GT(test, nr, 0);
		else
			KUNIT_EXPECT_EQ(test, nr, 0);

		/* The private list holds a private node's own zones only. */
		KUNIT_EXPECT_TRUE(test, zonelist_is_local(private, nid, &nr));
		if (node_state(nid, N_MEMORY_PRIVATE))
			KUNIT_EXPECT_GT(test, nr, 0);
		else
			KUNIT_EXPECT_EQ(test, nr, 0);
	}
	put_online_mems();
}

static void private_node_select_zonelist(struct kunit *test)
{
	int nid;

	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);

		KUNIT_EXPECT_PTR_EQ(test, select_zonelist(nid, GFP_KERNEL, 0),
				    &pgdat->node_zonelists[ZONELIST_FALLBACK]);
		KUNIT_EXPECT_PTR_EQ(test,
				    select_zonelist(nid, GFP_KERNEL | __GFP_THISNODE, 0),
				    &pgdat->node_zonelists[ZONELIST_NOFALLBACK]);
		KUNIT_EXPECT_PTR_EQ(test,
				    select_zonelist(nid, GFP_KERNEL | __GFP_THISNODE,
						    ALLOC_ZONELIST_PRIVATE),
				    &pgdat->node_zonelists[ZONELIST_PRIVATE]);
	}
}

/* A kernel-owned private node the task may allocate from, or NUMA_NO_NODE. */
static int private_test_node(void)
{
	int nid;

	for_each_node_state(nid, N_MEMORY_PRIVATE) {
		if (NODE_DATA(nid)->memory_owner != NODE_MEMORY_OWNER_KERNEL)
			continue;
		if (!cpusets_enabled() ||
		    node_isset(nid, cpuset_current_mems_allowed))
			return nid;
	}
	return NUMA_NO_NODE;
}

static const gfp_t private_test_gfp =
	GFP_HIGHUSER_MOVABLE | __GFP_NORETRY | __GFP_NOWARN;

/* An owner cookie that owns no node. */
static const char private_test_stranger;

static void expect_folio_on_node(struct kunit *test, struct folio *folio,
				 int nid)
{
	KUNIT_EXPECT_NOT_NULL(test, folio);
	if (!folio)
		return;
	KUNIT_EXPECT_EQ(test, folio_nid(folio), nid);
	folio_put(folio);
}

static void expect_no_folio(struct kunit *test, struct folio *folio)
{
	KUNIT_EXPECT_NULL(test, folio);
	if (folio)
		folio_put(folio);
}

static void expect_common_page(struct kunit *test, struct page *page)
{
	KUNIT_EXPECT_NOT_NULL(test, page);
	if (!page)
		return;
	KUNIT_EXPECT_TRUE(test, node_state(page_to_nid(page), N_MEMORY_COMMON));
	__free_pages(page, 0);
}

static void check_private_alloc(struct kunit *test, int nid)
{
	int common = first_node(node_states[N_MEMORY_COMMON]);
	nodemask_t mask = nodemask_of_node(nid);
	struct folio *folio;
	struct page *page;

	/* Only the owner allocates from the node, and only from that node. */
	folio = folio_alloc_private(NODE_MEMORY_OWNER_KERNEL, nid,
				    private_test_gfp, 0);
	expect_folio_on_node(test, folio, nid);
	expect_no_folio(test, folio_alloc_private(&private_test_stranger, nid,
						  private_test_gfp, 0));
	expect_no_folio(test, folio_alloc_private(NODE_MEMORY_OWNER_KERNEL,
						  common, private_test_gfp, 0));

	/* An MM-internal request lands on the node it names. */
	page = __alloc_frozen_pages(private_test_gfp, 0, nid, &mask,
				    ALLOC_ZONELIST_PRIVATE);
	KUNIT_EXPECT_NOT_NULL(test, page);
	if (page) {
		KUNIT_EXPECT_EQ(test, page_to_nid(page), nid);
		free_frozen_pages(page, 0);
	}
}

static void check_ordinary_alloc(struct kunit *test, int nid)
{
	nodemask_t mask = nodemask_of_node(nid);
	struct page *page;

	/* Naming the node, or a mask of only that node, falls back. */
	expect_common_page(test, alloc_pages_node(nid, private_test_gfp, 0));
	expect_common_page(test, __alloc_pages(private_test_gfp, 0, nid, &mask,
					       ALLOC_DEFAULT));

	/* A strict request has nowhere to go. */
	page = alloc_pages_node(nid, private_test_gfp | __GFP_THISNODE, 0);
	KUNIT_EXPECT_NULL(test, page);
	if (page)
		__free_pages(page, 0);
}

/* Run @check against a kernel-owned private node, under the hotplug lock. */
static void check_private_node(struct kunit *test,
			       void (*check)(struct kunit *test, int nid))
{
	int nid;

	get_online_mems();
	nid = private_test_node();
	if (nid != NUMA_NO_NODE)
		check(test, nid);
	put_online_mems();
	if (nid == NUMA_NO_NODE)
		kunit_skip(test, "no kernel-owned private node");
}

static void private_node_alloc_private(struct kunit *test)
{
	check_private_node(test, check_private_alloc);
}

static void private_node_alloc_ordinary(struct kunit *test)
{
	check_private_node(test, check_ordinary_alloc);
}

static struct kunit_case private_node_test_cases[] = {
	KUNIT_CASE(private_node_states_partition),
	KUNIT_CASE(private_node_features_valid),
	KUNIT_CASE(private_node_fallback_zonelists),
	KUNIT_CASE(private_node_local_zonelists),
	KUNIT_CASE(private_node_select_zonelist),
	KUNIT_CASE(private_node_alloc_private),
	KUNIT_CASE(private_node_alloc_ordinary),
	{}
};

static struct kunit_suite private_node_test_suite = {
	.name = "private_node",
	.test_cases = private_node_test_cases,
};
kunit_test_suite(private_node_test_suite);

MODULE_DESCRIPTION("KUnit tests for private memory node isolation");
MODULE_LICENSE("GPL");
