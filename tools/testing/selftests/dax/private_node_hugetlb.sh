#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# hugetlb allocation onto a private node is gated on CAP_HUGETLB
# (N_MEMORY_HUGETLB).  The global hugetlb pool distributes only across
# N_MEMORY_HUGETLB nodes, and the pool allocation reaches a private node only
# through the private zonelist.  hugetlb folios are unmovable, so the node is
# brought up kernel-zoned.
#
#   1. with CAP_HUGETLB: a global pool grow places hugepages on the private node.
#   2. without CAP_HUGETLB: the private node gets none (the alloc is gated off it).
#
# Needs a private node on a memoryless node + hugetlb; see private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_provision
[ -e "$D/mm_capabilities" ] || { ktap_skip_all "$DAX missing mm_capabilities attr"; exit "$KSFT_SKIP"; }
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
up() { pn_reset; pn_set hugetlb "$1"; pn_set reclaim 1; pn_hotplug online_kernel; }

N=64
SAVE=$(cat "$HP" 2>/dev/null)
trap 'echo "${SAVE:-0}" > "$HP" 2>/dev/null; pn_reset' EXIT
ktap_set_plan 2

# 1. CAP_HUGETLB: the private node participates in the global pool.
up 1
if [ "$(pn_state)" = online_kernel ] && pn_is_private; then
	got=$(pool_probe "$N")
	ktap_print_msg "CAP_HUGETLB=1: node$PN HugePages_Total=$got (pool=$N)"
	if [ "${got:-0}" -gt 0 ] 2>/dev/null; then
		ktap_test_pass "hugetlb pool placed $got hugepages on private node $PN via CAP_HUGETLB"
	else
		ktap_test_fail "hugetlb pool did not reach private node $PN with CAP_HUGETLB (got=$got)"
	fi
else
	ktap_test_skip "1 could not online $PN kernel-zoned private (hugetlb=1)"
fi

# 2. no CAP_HUGETLB: the private node is excluded from the pool.
up 0
if [ "$(pn_state)" = online_kernel ] && pn_is_private; then
	got=$(pool_probe "$N")
	ktap_print_msg "CAP_HUGETLB=0: node$PN HugePages_Total=$got (pool=$N)"
	if [ "${got:-1}" -eq 0 ] 2>/dev/null; then
		ktap_test_pass "hugetlb pool excluded private node $PN without CAP_HUGETLB (gated)"
	else
		ktap_test_fail "hugetlb pool reached private node $PN despite hugetlb cleared (got=$got)"
	fi
else
	ktap_test_skip "2 could not online $PN kernel-zoned private (hugetlb=0)"
fi

pn_reset
ktap_finished
