#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM convergence against a HOT resident set.
#
# cram_ratio.sh already covers the easy direction: a worse compression ratio
# inflates the balloon when the node has memory to give.  This covers the case
# where it does not.
#
# The balloon reserves by allocating, so its only reclaim is the allocator's,
# which honours references.  Fill the node with memory that is actively being
# read and every folio comes back FOLIOREF_ACTIVATE: the allocation fails and
# the balloon stops short of target.  For a fixed-size tier that is correct.
# For CRAM it is not -- a worse ratio means the physical backing is ALREADY
# gone, so a balloon that cannot converge leaves the device overcommitted,
# which is silent data loss.
#
# The convergence worker therefore escalates past polite reclaim: forced
# eviction ignoring references (reclaim_pages(), the MADV_PAGEOUT lever), then
# evacuation to DRAM for what cannot be swapped or dropped, then a node-scoped
# OOM kill when there is nothing to reclaim and nowhere to put it.
#
# Asserts the balloon reaches target anyway, and that forcing the eviction did
# not corrupt the data.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

DAX_BASE=/sys/bus/dax/devices
CRAM_DBG=/sys/kernel/debug/cram
ZRATIO=2000		# 2:1 configured
WORSE="1200 block"	# ratio + the driver's "block" urgency flag (converge_block)

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e "$CRAM_DBG/nodes" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$DIR/cram_converge_tool" ] ||
	{ ktap_skip_all "cram_converge_tool not built"; exit "$KSFT_SKIP"; }

cram_provision() {
	local d nid drv r

	pn_modprobe nd_e820 dax_pmem device_dax nd_pmem
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
		[ -e "$D/cram" ] && [ -e "$D/cram_zratio" ] && [ -e "$D/cram_compression_ratio" ] ||
			{ ktap_skip_all "$DAX missing cram ratio knobs"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		echo "$ZRATIO" > "$D/cram_zratio" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

# Deliberately swapless.  With swap available the allocator's own reclaim wins
# eventually -- referenced folios are activated, then deactivated, then taken --
# so the balloon converges without ever escalating and this test would prove
# nothing (it did exactly that on the first cut: forced=0 evacuated=0).
#
# With no swap, resident anon cannot be written anywhere: polite reclaim fails,
# and so does forced reclaim, because ignoring references does not conjure a
# swap device.  What is left is to MOVE the memory, which is the rung this
# covers.
pn_snapshot_swaps
swapoff -a 2>/dev/null

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
echo 1 > /sys/kernel/mm/numa/demotion_enabled 2>/dev/null
ktap_print_msg "cram node $PN online, zratio=$(awk -v n="$PN" '$1==n{print $2}' "$CRAM_DBG/nodes")"
# Release the tier before unplugging: unplugging a node that still holds
# resident folios makes offline retry migration instead of finishing.
trap 'sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; echo unplugged > "$D/state" 2>/dev/null' EXIT

ktap_set_plan 2

dmesg -C 2>/dev/null

# ---------------------------------------------------------------------------
# 1. the balloon converges with a hot resident set and no swap, by escalating
# ---------------------------------------------------------------------------
out=$("$DIR"/cram_converge_tool "$PN" "$D/cram_compression_ratio" "$WORSE" 60 2>&1)
rc=$?
case "$rc" in
0)	ktap_test_pass "converged by escalating, with no swap to fall back on [$out]" ;;
3)	ktap_test_skip "environmental: $out" ;;
*)	ktap_test_fail "balloon did not converge with hot memory resident (rc=$rc) [$out]" ;;
esac

# ---------------------------------------------------------------------------
# 2. no splat while forcing the eviction
# ---------------------------------------------------------------------------
splat=$(dmesg 2>/dev/null | grep -acE "KASAN|BUG:|Oops|WARNING:|refcount_t|list_del")
if [ "$splat" = 0 ]; then
	ktap_test_pass "no KASAN/BUG/UAF/WARN while forcing convergence"
else
	ktap_test_fail "kernel splat during forced convergence (count=$splat)"
	dmesg | grep -aE "KASAN|BUG:|Oops|WARNING:" | tail -5
fi

ktap_finished
