// SPDX-License-Identifier: GPL-2.0
/*
 * test_node_private - Test module for private NUMA node infrastructure
 *
 * Tests:
 *  1. Exclusive node acquisition
 *  2. node_private_register / unregister
 *  3. node_private_set_ops / clear_ops
 *  4. Flag and predicate queries
 *  5. Memory hotplug via add_private_memory_driver_managed()
 *  6. Allocation gating (__GFP_PRIVATE)
 *  7. Callback invocation (free_folio, folio_split, folio_migrate)
 *  8. Teardown and cleanup
 *
 * Usage:
 *   modprobe test_node_private [hotplug_start=0x<addr>] [hotplug_size=0x<size>]
 *
 * If hotplug_start/size are provided, the module will attempt to hotplug
 * that physical range as private memory.  Otherwise it only tests the
 * registration APIs.
 *
 * Results are printed to the kernel log (dmesg).
 */

#define pr_fmt(fmt) "test_node_private: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/numa.h>
#include <linux/nodemask.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/memory_hotplug.h>
#include <linux/node_private.h>
#include <linux/migrate.h>

static unsigned long hotplug_start;
module_param(hotplug_start, ulong, 0444);
MODULE_PARM_DESC(hotplug_start, "Physical start address for memory hotplug test");

static unsigned long hotplug_size;
module_param(hotplug_size, ulong, 0444);
MODULE_PARM_DESC(hotplug_size, "Size in bytes for memory hotplug test");

static int test_nid = NUMA_NO_NODE;
static struct node_private test_np;
static bool node_registered;
static bool memory_added;

/* Callback counters */
static atomic_t free_folio_count = ATOMIC_INIT(0);
static atomic_t folio_split_count = ATOMIC_INIT(0);
static atomic_t folio_migrate_count = ATOMIC_INIT(0);
static atomic_t memory_failure_count = ATOMIC_INIT(0);

static bool test_free_folio(struct folio *folio)
{
	atomic_inc(&free_folio_count);
	pr_info("  free_folio callback: folio pfn=%lu order=%u\n",
		folio_pfn(folio), folio_order(folio));
	return false; /* let buddy reclaim it */
}

static void test_folio_split(struct folio *folio, struct folio *new_folio)
{
	atomic_inc(&folio_split_count);
	pr_info("  folio_split callback: folio pfn=%lu new=%s\n",
		folio_pfn(folio), new_folio ? "yes" : "NULL(final)");
}

static void test_folio_migrate(struct folio *src, struct folio *dst)
{
	atomic_inc(&folio_migrate_count);
	pr_info("  folio_migrate callback: src pfn=%lu dst pfn=%lu\n",
		folio_pfn(src), folio_pfn(dst));
}

static void test_memory_failure(struct folio *folio, unsigned long pfn,
				int mf_flags)
{
	atomic_inc(&memory_failure_count);
	pr_info("  memory_failure callback: pfn=%lu flags=0x%x\n",
		pfn, mf_flags);
}

static int test_migrate_to(struct list_head *folios, int nid,
			   enum migrate_mode mode,
			   enum migrate_reason reason,
			   unsigned int *nr_succeeded)
{
	pr_info("  migrate_to callback: nid=%d\n", nid);
	if (nr_succeeded)
		*nr_succeeded = 0;
	return 0;
}

static const struct node_private_ops test_ops = {
	.free_folio = test_free_folio,
	.folio_split = test_folio_split,
	.migrate_to = test_migrate_to,
	.folio_migrate = test_folio_migrate,
	.memory_failure = test_memory_failure,
	.flags = NP_OPS_MIGRATION | NP_OPS_MEMPOLICY,
};

static int passed;
static int failed;

#define TEST(name, cond) do { \
	if (cond) { \
		pr_info("PASS: %s\n", name); \
		passed++; \
	} else { \
		pr_err("FAIL: %s\n", name); \
		failed++; \
	} \
} while (0)

static void test_registration(void)
{
	int ret;

	pr_info("=== Test: Registration API ===\n");

	/* Request exclusive node */
	test_nid = numa_request_exclusive_node();
	TEST("exclusive node acquired", test_nid != NUMA_NO_NODE);
	if (test_nid == NUMA_NO_NODE) {
		pr_err("No exclusive nodes available, skipping remaining tests\n");
		pr_err("Ensure CONFIG_ACPI_NUMA_STANDBY_NODES >= 1 and boot "
		       "with multiple NUMA nodes\n");
		return;
	}
	pr_info("  Got exclusive node %d\n", test_nid);

	/* Node should not be N_MEMORY yet */
	TEST("exclusive node not N_MEMORY", !node_state(test_nid, N_MEMORY));

	/* Register */
	test_np.owner = THIS_MODULE;
	ret = node_private_register(test_nid, &test_np);
	TEST("node_private_register succeeds", ret == 0);
	if (ret) {
		pr_err("  register failed: %d\n", ret);
		numa_release_exclusive_node(test_nid);
		test_nid = NUMA_NO_NODE;
		return;
	}
	node_registered = true;

	/* Node is not yet N_MEMORY_PRIVATE until memory is hotplugged */
	TEST("node not yet private (no memory)", !node_is_private(test_nid));
	/* But pgdat->private is set */
	TEST("pgdat->private is set",
	     rcu_access_pointer(NODE_DATA(test_nid)->private) == &test_np);

	/* Re-registration with same np should succeed (no-op) */
	ret = node_private_register(test_nid, &test_np);
	TEST("re-registration succeeds", ret == 0);

	/* Registration with different np should fail */
	{
		struct node_private other_np = {};
		ret = node_private_register(test_nid, &other_np);
		TEST("different np registration fails with -EBUSY", ret == -EBUSY);
	}

	/* Set ops */
	ret = node_private_set_ops(test_nid, &test_ops);
	TEST("node_private_set_ops succeeds", ret == 0);

	/* Check flags */
	TEST("NP_OPS_MIGRATION flag set",
	     node_private_has_flag(test_nid, NP_OPS_MIGRATION));
	TEST("NP_OPS_MEMPOLICY flag set",
	     node_private_has_flag(test_nid, NP_OPS_MEMPOLICY));
}

static void test_alloc_gating(void)
{
	struct zone *zone;
	int z;

	pr_info("=== Test: Allocation Gating ===\n");

	if (test_nid == NUMA_NO_NODE || !node_registered)
		return;

	/* Check zone_private_alloc_allowed for zones on this node */
	for (z = 0; z < MAX_NR_ZONES; z++) {
		zone = &NODE_DATA(test_nid)->node_zones[z];
		if (!populated_zone(zone))
			continue;

		TEST("zone_private_alloc_allowed rejects without GFP_PRIVATE",
		     !zone_private_alloc_allowed(zone, GFP_KERNEL));
		TEST("zone_private_alloc_allowed allows with GFP_PRIVATE",
		     zone_private_alloc_allowed(zone, GFP_KERNEL | __GFP_PRIVATE));
		break;
	}

	/* Non-private node zones should always be allowed */
	zone = &NODE_DATA(0)->node_zones[ZONE_NORMAL];
	if (populated_zone(zone)) {
		TEST("non-private zone always allowed",
		     zone_private_alloc_allowed(zone, GFP_KERNEL));
	}
}

static void test_memory_hotplug(void)
{
	int ret;

	pr_info("=== Test: Memory Hotplug ===\n");

	if (test_nid == NUMA_NO_NODE || !node_registered)
		return;

	if (!hotplug_start || !hotplug_size) {
		pr_info("  Skipping: no hotplug_start/hotplug_size provided\n");
		return;
	}

	pr_info("  Hotplugging 0x%lx bytes at 0x%lx to node %d\n",
		hotplug_size, hotplug_start, test_nid);

	ret = add_private_memory_driver_managed(test_nid, hotplug_start,
						hotplug_size,
						"System RAM (test_node_private)",
						MHP_NONE,
						MMOP_ONLINE_MOVABLE, &test_np);
	TEST("add_private_memory_driver_managed succeeds", ret == 0);
	if (ret) {
		pr_err("  hotplug failed: %d\n", ret);
		return;
	}
	memory_added = true;

	/* Node should now be N_MEMORY */
	TEST("node is N_MEMORY after hotplug", node_state(test_nid, N_MEMORY));
	TEST("node is still private after hotplug", node_is_private(test_nid));
}

static void test_alloc_private_pages(void)
{
	struct page *page;
	struct folio *folio;

	pr_info("=== Test: Private Page Allocation ===\n");

	if (test_nid == NUMA_NO_NODE || !memory_added)
		return;

	/* Without __GFP_PRIVATE, should NOT get pages from private node */
	page = alloc_pages_node(test_nid, GFP_KERNEL | __GFP_THISNODE |
				__GFP_NOWARN | __GFP_NORETRY, 0);
	TEST("alloc without GFP_PRIVATE on private node fails", page == NULL);
	if (page)
		__free_pages(page, 0);

	/* With __GFP_PRIVATE, should succeed */
	page = alloc_pages_node(test_nid, GFP_KERNEL | __GFP_THISNODE |
				__GFP_PRIVATE, 0);
	TEST("alloc with GFP_PRIVATE on private node succeeds", page != NULL);
	if (page) {
		folio = page_folio(page);
		TEST("allocated page is on private node",
		     page_to_nid(page) == test_nid);
		TEST("folio_is_private_node is true",
		     folio_is_private_node(folio));
		TEST("folio_is_private_managed is true",
		     folio_is_private_managed(folio));

		/* Free should trigger free_folio callback */
		atomic_set(&free_folio_count, 0);
		__free_pages(page, 0);
		TEST("free_folio callback fired",
		     atomic_read(&free_folio_count) > 0);
	}
}

static void test_cleanup_ops(void)
{
	int ret;

	pr_info("=== Test: Cleanup ===\n");

	if (test_nid == NUMA_NO_NODE || !node_registered)
		return;

	ret = node_private_clear_ops(test_nid, &test_ops);
	TEST("node_private_clear_ops succeeds", ret == 0);

	TEST("flags cleared after clear_ops",
	     !node_private_has_flag(test_nid, NP_OPS_MIGRATION));
}

static int __init test_node_private_init(void)
{
	pr_info("Starting private node infrastructure tests\n");

	test_registration();
	test_alloc_gating();
	test_memory_hotplug();
	test_alloc_private_pages();

	/*
	 * When built-in, keep ops registered so syzkaller can exercise
	 * private node code paths (NP_OPS_MEMPOLICY, NP_OPS_MIGRATION).
	 * When loadable, test cleanup now; full teardown happens in exit.
	 */
	if (!IS_BUILTIN(CONFIG_TEST_NODE_PRIVATE))
		test_cleanup_ops();

	pr_info("=== RESULTS: %d passed, %d failed ===\n", passed, failed);

	if (failed) {
		if (IS_BUILTIN(CONFIG_TEST_NODE_PRIVATE)) {
			pr_warn("Tests failed but continuing (built-in)\n");
			return 0;
		}
		return -EINVAL;
	}

	return 0;
}

static void __exit test_node_private_exit(void)
{
	int ret;

	if (memory_added && hotplug_start && hotplug_size) {
		ret = offline_and_remove_private_memory(test_nid, hotplug_start,
							hotplug_size);
		if (ret)
			pr_err("Failed to remove hotplugged memory: %d\n", ret);
		else
			pr_info("Removed hotplugged memory\n");
	}

	if (node_registered) {
		ret = node_private_unregister(test_nid);
		if (ret)
			pr_err("Failed to unregister node: %d\n", ret);
		else
			pr_info("Unregistered private node %d\n", test_nid);
	}

	if (test_nid != NUMA_NO_NODE) {
		numa_release_exclusive_node(test_nid);
		pr_info("Released exclusive node %d\n", test_nid);
	}

	pr_info("Callback totals: free_folio=%d split=%d migrate=%d failure=%d\n",
		atomic_read(&free_folio_count),
		atomic_read(&folio_split_count),
		atomic_read(&folio_migrate_count),
		atomic_read(&memory_failure_count));

	pr_info("Module unloaded\n");
}

module_init(test_node_private_init);
module_exit(test_node_private_exit);

MODULE_AUTHOR("Gregory Price <gourry@gourry.net>");
MODULE_DESCRIPTION("Test module for private NUMA node infrastructure");
MODULE_LICENSE("GPL");
