# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -O2

all:

TEST_GEN_FILES := private_node_tool
TEST_GEN_FILES += cram_readable_tool
TEST_GEN_FILES += cram_fork_tool
TEST_GEN_FILES += cram_ops_tool
TEST_GEN_FILES += cram_pgcache_tool
TEST_GEN_FILES += cram_swap_tool
TEST_GEN_FILES += cram_pressure_tool
TEST_GEN_FILES += cram_leak_tool
TEST_GEN_FILES += cram_perf_tool
TEST_GEN_FILES += cram_file_perf_tool
TEST_GEN_FILES += cram_gate_tool
TEST_GEN_FILES += cram_hot_tool
TEST_GEN_FILES += cram_coherence_tool
TEST_GEN_FILES += cram_pgcache_large_tool
TEST_GEN_FILES += cram_readahead_tool

TEST_PROGS := dax-kmem-hotplug.sh
TEST_PROGS += dax-kmem-dax_file.sh
TEST_PROGS += private_node_hotplug.sh
TEST_PROGS += private_node_isolation.sh
TEST_PROGS += private_node_ltpin.sh
TEST_PROGS += private_node_pressure.sh
TEST_PROGS += private_node_reclaim.sh
TEST_PROGS += private_node_compaction.sh
TEST_PROGS += private_node_cpuset_default_open.sh
TEST_PROGS += private_node_mbind.sh
TEST_PROGS += private_node_mempolicy.sh
TEST_PROGS += private_node_mempolicy_rebind.sh
TEST_PROGS += private_node_move_pages.sh
TEST_PROGS += private_node_migrate_pages.sh
TEST_PROGS += private_node_demotion.sh
TEST_PROGS += private_node_hotplug_transitions.sh
TEST_PROGS += private_node_observability.sh
TEST_PROGS += private_node_thp.sh
TEST_PROGS += private_node_pagetables.sh
TEST_PROGS += private_node_adistance.sh
TEST_PROGS += cram_node_basic.sh
TEST_PROGS += cram_readable.sh
TEST_PROGS += cram_swap.sh
TEST_PROGS += cram_pgcache.sh
TEST_PROGS += cram_coherence.sh
TEST_PROGS += cram_pgcache_large.sh
TEST_PROGS += cram_readahead.sh
TEST_PROGS += cram_fork.sh
TEST_PROGS += cram_ops.sh
TEST_PROGS += cram_pressure.sh
TEST_PROGS += cram_leak.sh
TEST_PROGS += cram_perf.sh
TEST_PROGS += cram_file_perf.sh
TEST_PROGS += cram_fs_matrix.sh
TEST_PROGS += cram_ratio.sh
TEST_PROGS += cram_gate.sh
TEST_PROGS += cram_hot.sh
TEST_PROGS += cram_trim.sh
TEST_PROGS += cram_control.sh

# generic tiering perf tooling (uniform across the matrix)
TEST_GEN_FILES += dax_microbench_tool
TEST_GEN_FILES += tier_latency_tool
TEST_PROGS += dax_microbench.sh
TEST_PROGS += tier_latency.sh
TEST_FILES += collect_stats.sh dcperf_taobench.sh

include ../lib.mk

$(OUTPUT)/cram_pressure_tool: LDLIBS += -lpthread
$(OUTPUT)/cram_leak_tool: LDLIBS += -lpthread
