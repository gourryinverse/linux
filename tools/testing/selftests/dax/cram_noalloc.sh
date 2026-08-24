#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# ZONE_NO_ALLOC: a zone's owner can withdraw it from the page allocator.
#
# The property under test is the one that makes the flag worth having: while
# it is set, NOTHING new lands on the zone, and what is already resident is
# left alone.  Both halves matter.  A mechanism that only stopped new
# allocations but purged the residents would be cpuset.mems, which is why that
# route was rejected; a mechanism that let allocations through under pressure
# would be a preference, and the two users that want this -- a compressed tier
# whose backing is gone, a guest whose host cannot back the memory -- both mean
# it absolutely.
#
# Driven through the [TEST] debugfs verb rather than by starving a real device,
# so the flag is exercised on its own rather than through a driver's policy.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

DAX_BASE=/sys/bus/dax/devices
CRAM_DBG=/sys/kernel/debug/cram

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e "$CRAM_DBG/nodes" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$DIR/cram_demote_tool" ] || [ -x "$DIR/cram_readable_tool" ] ||
	{ ktap_skip_all "cram tools not built"; exit "$KSFT_SKIP"; }

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
		[ -e "$D/cram" ] ||
			{ ktap_skip_all "$DAX has no cram knob"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

# free pages on the cram node, from cram/nodes
cram_free() { awk -v n="$PN" '$1==n {print $9}' "$CRAM_DBG/nodes"; }
noalloc()   { echo "noalloc $PN $1" > "$CRAM_DBG/control"; }

pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
echo 1 > /sys/kernel/mm/numa/demotion_enabled 2>/dev/null
ktap_print_msg "cram node $PN online, free=$(cram_free)"
trap 'noalloc 0 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; echo unplugged > "$D/state" 2>/dev/null' EXIT

ktap_set_plan 2
dmesg -C 2>/dev/null

# ---------------------------------------------------------------------------
# 1. withdrawn means withdrawn, and only for new placement
# ---------------------------------------------------------------------------
out=$("$DIR"/cram_noalloc_tool "$PN" 64 2>&1)
rc=$?
case "$rc" in
0)	ktap_test_pass "withdrawn zone refused placement and kept its residents [$out]" ;;
3)	ktap_test_skip "environmental: $out" ;;
*)	ktap_test_fail "ZONE_NO_ALLOC contract broken (rc=$rc) [$out]" ;;
esac

# ---------------------------------------------------------------------------
# 2. no splat
# ---------------------------------------------------------------------------
splat=$(dmesg 2>/dev/null | grep -acE "KASAN|BUG:|Oops|WARNING:")
if [ "$splat" = 0 ]; then
	ktap_test_pass "no KASAN/BUG/UAF/WARN while the zone was withdrawn"
else
	ktap_test_fail "kernel splat with the zone withdrawn (count=$splat)"
	dmesg | grep -aE "KASAN|BUG:|Oops|WARNING:" | tail -5
fi

ktap_finished
