#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM writeback-to-physical-swap test.
#
# Fills the CRAM node with demoted anonymous folios, then balloon-inflates past
# its free capacity so direct reclaim must swap resident CRAM folios out to the
# physical swap device.  Asserts pswpout rises, pages leave CRAM to swap, and the
# data survives swap-in.  Use a SMALL cram node (e.g. 256M) so it fills quickly.
#
# Provision a dax device on a memoryless node via memmap= (see vng.workflow).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
TOOL="$DIR/cram_swap_tool"

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -d "$CRAM_DBG" ] && [ -e "$CRAM_DBG/demote_count" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_swap_tool not built"; exit "$KSFT_SKIP"; }

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
		[ -e "$D/cram" ] ||
			{ ktap_skip_all "$DAX has no 'cram' knob"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node (see header)"
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

ktap_set_plan 2

dmesg -C 2>/dev/null
"$TOOL" "$PN" 2>&1 | tee /tmp/cram_swap.out
rc=${PIPESTATUS[0]}
out=$(cat /tmp/cram_swap.out)
splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:|modified in place")

# 1. resident CRAM folios are written back to physical swap and read back intact
if [ "$rc" = 0 ]; then
	ktap_test_pass "writeback-to-swap: resident CRAM folios swapped out + swapped in intact"
elif [ "$rc" = 3 ]; then
	ktap_test_skip "could not fill CRAM node (environmental): $out"
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
