#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# hugetlb allocation onto a private node is gated on FEAT_USER_NUMA
# (N_MEMORY_USER_NUMA).  Every route into the pool is userspace naming a node
# -- per-node nr_hugepages, the global pool, nr_hugepages_mempolicy -- so pool
# placement is a userland NUMA control and rides that bit rather than one of
# its own.  The pool reaches a private node only through the private zonelist,
# and hugetlb folios are unmovable, so the node is brought up kernel-zoned.
#
#   1. with FEAT_USER_NUMA: a global pool grow places hugepages on the node.
#   2. without it: the private node gets none (the alloc is gated off it).
#
# Needs a private node on a memoryless node + hugetlb; see private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_provision
HP=/proc/sys/vm/nr_hugepages
[ -w "$HP" ] || { ktap_skip_all "no hugetlb pool control ($HP)"; exit "$KSFT_SKIP"; }

# HugePages_Total for the private node, from its per-node meminfo.
hpt() { awk '/HugePages_Total/{print $NF}' "$NODE_BASE/node$PN/meminfo" 2>/dev/null; }
# grow the global pool to $N, echo the private node's resulting share, drain.
pool_probe() {
	echo 0 > "$HP" 2>/dev/null
	echo "$1" > "$HP" 2>/dev/null; sleep 1
	hpt
	echo 0 > "$HP" 2>/dev/null
}

# hugetlb pages are unmovable: bring the node up kernel-zoned.
up() { pn_reset; pn_set user_numa "$1"; pn_set reclaim 1; pn_hotplug online_kernel; }

N=64
SAVE=$(cat "$HP" 2>/dev/null)
trap 'echo "${SAVE:-0}" > "$HP" 2>/dev/null; pn_reset' EXIT
ktap_set_plan 2

# 1. FEAT_USER_NUMA: the private node participates in the global pool.
up 1
if [ "$(pn_state)" = online_kernel ] && pn_is_private; then
	got=$(pool_probe "$N")
	ktap_print_msg "FEAT_USER_NUMA=1: node$PN HugePages_Total=$got (pool=$N)"
	if [ "${got:-0}" -gt 0 ] 2>/dev/null; then
		ktap_test_pass "hugetlb pool placed $got hugepages on private node $PN via FEAT_USER_NUMA"
	else
		ktap_test_fail "hugetlb pool did not reach private node $PN with FEAT_USER_NUMA (got=$got)"
	fi
else
	ktap_test_skip "1 could not online $PN kernel-zoned private (user_numa=1)"
fi

# 2. no FEAT_USER_NUMA: the private node is excluded from the pool.
up 0
if [ "$(pn_state)" = online_kernel ] && pn_is_private; then
	got=$(pool_probe "$N")
	ktap_print_msg "FEAT_USER_NUMA=0: node$PN HugePages_Total=$got (pool=$N)"
	if [ "${got:-1}" -eq 0 ] 2>/dev/null; then
		ktap_test_pass "hugetlb pool excluded private node $PN without FEAT_USER_NUMA"
	else
		ktap_test_fail "hugetlb pool reached private node $PN despite user_numa cleared (got=$got)"
	fi
else
	ktap_test_skip "2 could not online $PN kernel-zoned private (user_numa=0)"
fi

pn_reset
ktap_finished
