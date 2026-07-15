# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -O2

all:

TEST_GEN_FILES := private_node_tool

TEST_PROGS := dax-kmem-hotplug.sh
TEST_PROGS += private_node_hotplug.sh
TEST_PROGS += private_node_isolation.sh
TEST_PROGS += private_node_pressure.sh
TEST_PROGS += private_node_reclaim.sh
TEST_PROGS += private_node_compaction.sh
TEST_PROGS += private_node_cma.sh
TEST_PROGS += private_node_contig.sh
TEST_PROGS += private_node_cpuset_governed.sh
TEST_PROGS += private_node_cpuset_fallback.sh
TEST_PROGS += private_node_mempolicy.sh
TEST_PROGS += private_node_mempolicy_rebind.sh
TEST_PROGS += private_node_mempolicy_relative.sh
TEST_PROGS += private_node_mempolicy_privonly.sh
TEST_PROGS += private_node_move_pages.sh
TEST_PROGS += private_node_migrate_pages.sh
TEST_PROGS += private_node_demotion.sh
TEST_PROGS += private_node_hotplug_transitions.sh
TEST_PROGS += private_node_unbind_rebind.sh
TEST_PROGS += private_node_features_claim.sh
TEST_PROGS += private_node_observability.sh
TEST_PROGS += private_node_thp.sh private_node_hugetlb.sh
TEST_PROGS += private_node_pagetables.sh

# generic tiering perf tooling (uniform across the matrix)
TEST_GEN_FILES += dax_microbench_tool
TEST_GEN_FILES += tier_latency_tool
TEST_PROGS += dax_microbench.sh
TEST_PROGS += tier_latency.sh
TEST_FILES += collect_stats.sh dcperf_taobench.sh

include ../lib.mk
