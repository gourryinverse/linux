# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -O2

all:

TEST_GEN_FILES := private_node_tool

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

include ../lib.mk
