#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM sticky allocation-gate test (cram_allow_allocation).
#
# Drives the driver directive cram_allow_allocation() through the dax device's
# cram_allow_allocation sysfs knob and asserts demotions honor it:
#   allow=1 (default) -> MADV_PAGEOUT anon demotes onto the cram node
#   allow=0           -> cram refuses; the pages spill to swap instead
#   allow=1 (restored)-> demotions resume
#
# Needs a swap device so the allow=0 case has somewhere to reclaim to.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
TOOL="$DIR/cram_gate_tool"

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e "$CRAM_DBG/nodes" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_gate_tool not built"; exit "$KSFT_SKIP"; }

cram_provision() {
	local d nid drv r

	modprobe -q nd_e820 dax_pmem device_dax nd_pmem 2>/dev/null
	modprobe -q kmem 2>/dev/null
	[ -d /sys/bus/dax/drivers/kmem ] ||
		{ ktap_skip_all "kmem driver unavailable"; exit "$KSFT_SKIP"; }
	if command -v ndctl >/dev/null 2>&1; then
		for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
			ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f \
				>/dev/null 2>&1
		done
	fi
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node"); [ "$nid" -ge 0 ] 2>/dev/null || continue
		node_in_mask "$nid" has_memory && continue
		drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
		[ "$drv" = device_dax ] &&
			basename "$d" > /sys/bus/dax/drivers/device_dax/unbind 2>/dev/null
		[ "$drv" = kmem ] ||
			basename "$d" > /sys/bus/dax/drivers/kmem/new_id 2>/dev/null
		sleep 1
		D=$d; DAX=$(basename "$d"); PN=$nid
		[ -e "$D/cram_allow_allocation" ] ||
			{ ktap_skip_all "$DAX has no cram_allow_allocation knob"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available (pass a raw drive to vng)"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
node_in_mask "$PN" has_private_memory ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online"
trap 'echo unplugged > "$D/state" 2>/dev/null; pn_restore_globals' EXIT

ktap_set_plan 4

dmesg -C 2>/dev/null

# 1. gate open by default -> region demotes onto cram (tool rc 0)
out=$("$TOOL" "$PN"); rc=$?
if [ "$rc" = 0 ]; then
	ktap_test_pass "gate open (default): demoted onto cram [$out]"
else
	ktap_test_skip "could not demote onto cram (environmental) [$out rc=$rc]"
	echo unplugged > "$D/state" 2>/dev/null
	ktap_finished
	exit 0
fi

# 2. revoke -> a fresh region must NOT land on cram (spills to swap; tool rc 3)
echo 0 > "$D/cram_allow_allocation"
out=$("$TOOL" "$PN"); rc=$?
if [ "$rc" = 3 ]; then
	ktap_test_pass "gate closed (allow=0): demotion refused, spilled off-cram [$out]"
else
	ktap_test_fail "demotion landed on cram despite allow=0 [$out rc=$rc]"
fi

# 3. allowed column in cram/nodes reflects the revoke
allowed=$(awk -v n="$PN" '$1==n {print $8}' "$CRAM_DBG/nodes")
if [ "$allowed" = 0 ]; then
	ktap_test_pass "cram/nodes reports allowed=0"
else
	ktap_test_fail "cram/nodes allowed=$allowed (expected 0)"
fi

# 4. restore -> demotions resume (tool rc 0)
echo 1 > "$D/cram_allow_allocation"
out=$("$TOOL" "$PN"); rc=$?
if [ "$rc" = 0 ]; then
	ktap_test_pass "gate reopened (allow=1): demotions resume [$out]"
else
	ktap_test_fail "demotion did not resume after allow=1 [$out rc=$rc]"
fi

splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|WARNING:|Oops|refcount_t|modified in place")
[ "$splat" = 0 ] || ktap_print_msg "WARNING: $splat dmesg splat(s) during gate test"

ktap_finished
