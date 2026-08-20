#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# private_node= and the DMA contiguous allocator cannot share a node.
#
# cma= and numa_cma= reserve from memblock before pgdats exist, and
# private_node= marks its node during free_area_init(), so a CMA area can end
# up on a node that afterwards becomes private.  cma_alloc() is PFN-addressed,
# so dma_alloc_contiguous() would then hand device memory to any driver asking
# for DMA memory, with the zonelist never consulted.  The node is refused
# instead.
#
#   1. A node named by both cma=/numa_cma= and private_node= is NOT private.
#   2. The refusal says which parameter collided.
#   3. hugetlb_cma= does not trigger it: those areas are per node and are
#      drawn on only by the HugeTLB gigantic path, which applies the pool's
#      own node policy.
#
# Everything is decided at boot, so each cell skips unless the command line
# set up the case it covers.  nf_vng_cma.sh boots all three at once.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CMDLINE=$(cat /proc/cmdline)

# Nodes named by private_node= on the command line.
private_node_args() {
	tr ' ' '\n' <<<"$CMDLINE" | sed -n 's/^private_node=\([0-9]*\).*/\1/p'
}

# The node a cma=size@base lands on, resolved through the node's own range.
# Echoes nothing when cma= has no explicit base.
cma_base_node() {
	local base n
	base=$(tr ' ' '\n' <<<"$CMDLINE" | sed -n 's/^cma=[^@]*@\(0x[0-9a-fA-F]*\).*/\1/p')
	[ -n "$base" ] || return 1
	for n in "$NODE_BASE"/node[0-9]*; do
		local nid=${n##*node} blk bs first last
		[ -r "$n/meminfo" ] || continue
		bs=$(cat /sys/devices/system/memory/block_size_bytes)
		first=$(find "$n" -maxdepth 1 -name 'memory[0-9]*' -printf '%f\n' 2>/dev/null |
			sed 's/^memory//' | sort -n | head -1)
		last=$(find "$n" -maxdepth 1 -name 'memory[0-9]*' -printf '%f\n' 2>/dev/null |
		       sed 's/^memory//' | sort -n | tail -1)
		[ -n "$first" ] || continue
		if [ $(( base )) -ge $(( first * 0x$bs )) ] &&
		   [ $(( base )) -lt $(( (last + 1) * 0x$bs )) ]; then
			echo "$nid"; return 0
		fi
	done
	return 1
}

pn_begin
pn_require_root
pn_need_debugfs

ktap_set_plan 3

CMA_NODE=$(cma_base_node)
NUMA_CMA_NODE=$(tr ' ' '\n' <<<"$CMDLINE" | sed -n 's/^numa_cma=\([0-9]*\):.*/\1/p' | head -1)
DMA_CMA_NODE=${CMA_NODE:-$NUMA_CMA_NODE}

# ---------------------------------------------------------------------------
# 1. the colliding node is not private
# ---------------------------------------------------------------------------
if [ -z "$DMA_CMA_NODE" ]; then
	ktap_test_skip "1 no cma=/numa_cma= on the command line"
elif ! private_node_args | grep -qx "$DMA_CMA_NODE"; then
	ktap_test_skip "1 node $DMA_CMA_NODE not also named by private_node="
elif pn_node_is_private "$DMA_CMA_NODE"; then
	ktap_test_fail "1 node $DMA_CMA_NODE is private despite hosting a DMA CMA area"
else
	ktap_test_pass "1 node $DMA_CMA_NODE refused: hosts a DMA CMA area"
fi

# ---------------------------------------------------------------------------
# 2. the refusal names the reason
# ---------------------------------------------------------------------------
if [ -z "$DMA_CMA_NODE" ]; then
	ktap_test_skip "2 no cma=/numa_cma= on the command line"
elif dmesg | grep -q "private_node: node $DMA_CMA_NODE hosts a DMA CMA area"; then
	ktap_test_pass "2 refusal reported the colliding parameter"
else
	ktap_print_msg "$(dmesg | grep -i 'private_node:' | head -5)"
	ktap_test_fail "2 no DMA CMA refusal message for node $DMA_CMA_NODE"
fi

# ---------------------------------------------------------------------------
# 3. hugetlb_cma= does not cause a refusal
# ---------------------------------------------------------------------------
# Needs the area to be provably on the node being checked, so read the node
# back out of the reservation message rather than assuming where it landed.
HC_NODE=$(dmesg | sed -n 's/.*hugetlb_cma: reserved .* on node \([0-9]*\).*/\1/p' | head -1)
if ! grep -q 'hugetlb_cma=' /proc/cmdline; then
	ktap_test_skip "3 no hugetlb_cma= on the command line"
elif [ -z "$HC_NODE" ]; then
	ktap_print_msg "$(dmesg | grep -i hugetlb_cma | head -3)"
	ktap_test_skip "3 hugetlb_cma= reserved nothing (needs a multiple of the gigantic size)"
elif ! private_node_args | grep -qx "$HC_NODE"; then
	ktap_test_skip "3 hugetlb_cma landed on node $HC_NODE, which private_node= does not claim"
elif pn_node_is_private "$HC_NODE"; then
	ktap_test_pass "3 node $HC_NODE hosts a hugetlb_cma area and stayed private"
else
	ktap_test_fail "3 node $HC_NODE refused despite hugetlb_cma being the only area on it"
fi

ktap_finished
