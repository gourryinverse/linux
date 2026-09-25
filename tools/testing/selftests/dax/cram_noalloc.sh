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
# Driven through the cramdax provider's no_alloc signal so the flag is exercised
# directly rather than by starving a real device.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

DAX_BASE=/sys/bus/dax/devices

ktap_print_header
pn_require_root

[ -x "$DIR/cram_noalloc_tool" ] ||
	{ ktap_skip_all "cram_noalloc_tool not built"; exit "$KSFT_SKIP"; }

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
		[ -e "$D/no_alloc" ] && [ -e "$D/balloon_target" ] ||
			{ ktap_skip_all "$DAX missing CRAM controls"; exit "$KSFT_SKIP"; }
		echo offline > "$D/state" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

# Free pages on the CRAM node from the standard per-node vmstat.
cram_free() { pn_node_vmstat "$PN" nr_free_pages; }

pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online, free=$(cram_free)"
cleanup()
{
	echo 0 > "$D/no_alloc" 2>/dev/null
	echo 0 > "$D/balloon_target" 2>/dev/null
	sync
	echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
	echo offline > "$D/state" 2>/dev/null
}
trap cleanup EXIT

ktap_set_plan 2
dmesg -C 2>/dev/null

# ---------------------------------------------------------------------------
# 1. withdrawn means withdrawn, and only for new placement
# ---------------------------------------------------------------------------
out=$("$DIR"/cram_noalloc_tool "$PN" 64 "$D/no_alloc" \
	"$D/balloon_target" 2>&1)
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
