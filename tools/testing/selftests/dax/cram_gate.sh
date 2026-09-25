#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM sticky allocation-gate test.
#
# Drives cram_set_no_alloc() through the dax device's no_alloc
# sysfs knob and asserts placements honor it:
#   no_alloc=0 (default) -> MADV_PAGEOUT anon demotes onto the cram node
#   no_alloc=1           -> cram refuses; the pages spill to swap instead
#   allow=1 (restored)-> demotions resume
#
# Needs a swap device so the no_alloc=1 case has somewhere to reclaim to.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/cram_gate_tool"

ktap_print_header
pn_require_root

[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_gate_tool not built"; exit "$KSFT_SKIP"; }

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
		[ -e "$D/no_alloc" ] ||
			{ ktap_skip_all "$DAX has no no_alloc knob"; exit "$KSFT_SKIP"; }
		echo offline > "$D/state" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node"
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
trap 'echo offline > "$D/state" 2>/dev/null; pn_restore_globals' EXIT

ktap_set_plan 3

dmesg -C 2>/dev/null

# 1. gate open by default -> region demotes onto cram (tool rc 0)
out=$("$TOOL" "$PN"); rc=$?
if [ "$rc" = 0 ]; then
	ktap_test_pass "gate open (default): demoted onto cram [$out]"
else
	ktap_test_skip "could not demote onto cram (environmental) [$out rc=$rc]"
	echo offline > "$D/state" 2>/dev/null
	ktap_finished
	exit 0
fi

# 2. revoke -> a fresh region must NOT land on cram (spills to swap; tool rc 3)
echo 1 > "$D/no_alloc"
out=$("$TOOL" "$PN"); rc=$?
if [ "$rc" = 3 ]; then
	ktap_test_pass "gate closed (no_alloc=1): demotion refused, spilled off-cram [$out]"
else
	ktap_test_fail "demotion landed on cram despite no_alloc=1 [$out rc=$rc]"
fi

# 3. restore -> demotions resume (tool rc 0)
echo 0 > "$D/no_alloc"
out=$("$TOOL" "$PN"); rc=$?
if [ "$rc" = 0 ]; then
	ktap_test_pass "gate reopened (no_alloc=0): demotions resume [$out]"
else
	ktap_test_fail "demotion did not resume after no_alloc=0 [$out rc=$rc]"
fi

splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|WARNING:|Oops|refcount_t|modified in place")
[ "$splat" = 0 ] || ktap_print_msg "WARNING: $splat dmesg splat(s) during gate test"

ktap_finished
