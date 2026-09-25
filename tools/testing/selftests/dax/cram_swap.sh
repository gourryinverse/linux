#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM writeback-to-physical-swap test.
#
# Fills the CRAM node with demoted anonymous folios, then balloon-inflates past
# its free capacity and asserts the residents are written back to physical swap
# and survive swap-in.  Use a SMALL cram node (e.g. 256M) so it fills quickly.
#
# SKIPS unless CRAM_SWAP_FILL_DRAM=1.  The balloon acquires blocks with
# alloc_contig_range(), which clears them by migrating residents; while DRAM has
# room that succeeds and nothing reaches swap.  Forcing the eviction rung needs
# DRAM genuinely full, which this guest cannot do without thrashing -- see the
# comment in cram_swap_tool.c.
#
# Provision a dax device on a memoryless node via memmap= (see vng.workflow).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/cram_swap_tool"

ktap_print_header
pn_require_root

[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_swap_tool not built"; exit "$KSFT_SKIP"; }

cram_provision() {
	local d nid r

	pn_modprobe nd_e820 dax_pmem device_dax nd_pmem
	modprobe -q cramdax 2>/dev/null
	[ -d /sys/bus/dax/drivers/cramdax ] ||
		{ ktap_skip_all "cramdax driver unavailable"; exit "$KSFT_SKIP"; }
	if command -v ndctl >/dev/null 2>&1; then
		for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
			ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f \
				>/dev/null 2>&1
		done
	fi
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node"); [ "$nid" -ge 0 ] 2>/dev/null || continue
		pn_bind_cramdax "$d" || continue
		sleep 1
		D=$d; DAX=$(basename "$d"); PN=$nid
		[ -e "$D/state" ] ||
			{ ktap_skip_all "$DAX has no CRAM state control"; exit "$KSFT_SKIP"; }
		echo offline > "$D/state" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node (see header)"
	exit "$KSFT_SKIP"
}

pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available (pass a raw drive to vng)"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online"
cleanup()
{
	echo 0 > "$D/no_alloc" 2>/dev/null
	echo offline > "$D/state" 2>/dev/null
	pn_restore_globals
}
trap cleanup EXIT

ktap_set_plan 2

dmesg -C 2>/dev/null
"$TOOL" "$PN" "$D/balloon_target" "$D/no_alloc" 2>&1 |
	tee /tmp/cram_swap.out
rc=${PIPESTATUS[0]}
out=$(cat /tmp/cram_swap.out)
splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:|modified in place")

# 1. resident CRAM folios are written back to physical swap and read back intact
if [ "$rc" = 0 ]; then
	ktap_test_pass "writeback-to-swap: resident CRAM folios swapped out + swapped in intact"
elif [ "$rc" = 3 ]; then
	ktap_test_skip "environmental, see the diagnostic: $out"
else
	ktap_test_fail "writeback-to-swap failed (rc=$rc): $out"
fi

# 2. no splat during swap-out/swap-in
if [ "$splat" = 0 ]; then
	ktap_test_pass "no splat during CRAM writeback round trip"
else
	ktap_test_fail "kernel splat during CRAM writeback (count=$splat)"
fi

ktap_finished
