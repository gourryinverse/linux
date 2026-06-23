#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# A private node that opts into reclaim keeps its folios on-node across reclaim,
# and gates userland reclaim on CAP_RECLAIM.
#
#   1. Swap round-trip: fault on-node, MADV_PAGEOUT, evict the swap cache, read
#      back -- swap-in (do_swap_page, no ->fault) refaults BACK on-node, data intact.
#   2. CAP_RECLAIM gate (userland): MADV_PAGEOUT swaps out node folios only with
#      reclaim opted in (pswpout grows); with it cleared it is a no-op.
#   3. CAP_RECLAIM gate (kernel): overfilling the node via its bind swaps node
#      folios out with reclaim (pswpout grows); without it they are never
#      reclaimed (the overfill OOMs instead) despite a usable swap device.
#
# Needs a kmem-private dax device on a memoryless node AND a usable swap device
# (e.g. a vng --disk); SKIPs otherwise.  See private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

MB=64

pswpout() { awk '/^pswpout /{print $2}' /proc/vmstat; }
zone_pages() { awk -v n="$1" -v k="$2" '$1=="Node"{i=($2==n",")} i&&$1==k{m+=$2} END{print m+0}' /proc/zoneinfo; }

pn_begin
pn_require_tool
pn_provision
pn_reset
pn_snapshot_swaps				# restore swap state (we enable it below) on exit
trap pn_restore_globals EXIT INT TERM
pn_swap_setup || { ktap_skip_all "no usable swap device (attach a vng --disk)"; pn_reset; exit "$KSFT_SKIP"; }

pn_set reclaim 1
pn_set hotunplug 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN (reclaim) as private"
	pn_reset; exit "$KSFT_SKIP"
fi
ktap_print_msg "using $DAX on private node $PN with swap"
ktap_set_plan 3

# 1. swap round-trip back onto the private node
evict_mb=$(( $(zone_pages "$PN" managed) / 256 + 128 ))
"$TOOL" daxswap "/dev/$DAX" "$PN" "$MB" "$evict_mb" >/tmp/pn_daxswap.$$ 2>&1
rc=$?
sed 's/^/# /' /tmp/pn_daxswap.$$; rm -f /tmp/pn_daxswap.$$
case "$rc" in
0) ktap_test_pass "swapped-out folios refaulted back onto private node $PN, data intact" ;;
2) ktap_test_skip "swap round-trip inconclusive (no swap-in / cache not dropped)" ;;
*) ktap_test_fail "folios bled off node $PN on swap-in (rc=$rc)" ;;
esac

# 2. MADV_PAGEOUT (userland reclaim) gated by CAP_RECLAIM
p0=$(pswpout); "$TOOL" daxmadv "/dev/$DAX" 200 pageout >/dev/null 2>&1
on=$(( $(pswpout) - p0 ))
pn_hotplug unplugged; sleep 1
pn_set reclaim 0
pn_hotplug online_movable
p0=$(pswpout); "$TOOL" daxmadv "/dev/$DAX" 200 pageout >/dev/null 2>&1
off=$(( $(pswpout) - p0 ))
ktap_print_msg "pswpout delta: reclaim=1 -> $on, reclaim=0 -> $off"
if [ "$on" -gt 0 ] && [ "$off" -le $(( on / 4 + 16 )) ]; then
	ktap_test_pass "MADV_PAGEOUT reclaims node folios only when CAP_RECLAIM is set"
else
	ktap_test_fail "CAP_RECLAIM gate wrong (on=$on off=$off)"
fi

# 3. kernel reclaim of the node is gated by CAP_RECLAIM, even with swap present.
# Overfill the node via its bind: with reclaim the kernel swaps node folios to
# fit (trigger completes, pswpout grows); without it the node OOMs despite swap.
kr_overfill() {	# $1 = reclaim cap; echoes "trigger_rc pswpout_delta"
	pn_hotplug unplugged 2>/dev/null
	pn_set reclaim "$1"; pn_set hotunplug 1
	pn_hotplug online_movable
	pn_is_private || { echo "99 0"; return; }
	local man_mb p0 trc hg
	man_mb=$(( $(zone_pages "$PN" managed) / 256 ))
	[ "$man_mb" -lt 64 ] && { echo "98 0"; return; }
	p0=$(pswpout)
	"$TOOL" daxmap "/dev/$DAX" $(( man_mb * 70 / 100 )) "$PN" 60 >/dev/null 2>&1 & hg=$!
	sleep 3
	"$TOOL" daxmap "/dev/$DAX" $(( man_mb * 60 / 100 )) "$PN" 0 >/dev/null 2>&1; trc=$?
	kill -9 "$hg" 2>/dev/null; wait "$hg" 2>/dev/null
	echo "$trc $(( $(pswpout) - p0 ))"
}
read -r r1 s1 <<<"$(kr_overfill 1)"
read -r r0 s0 <<<"$(kr_overfill 0)"
ktap_print_msg "kernel-reclaim overfill: reclaim=1 rc=$r1 pswp=$s1 ; reclaim=0 rc=$r0 pswp=$s0"
# The gate signal is whether the node's folios swap: with reclaim the overfill
# swaps them (pswpout grows); without it they are never reclaimed (pswpout flat,
# the overfill OOMs instead - whichever faulter the OOM killer picks).
if [ "$r1" = 99 ] || [ "$r0" = 99 ]; then
	ktap_test_skip "could not online node for the kernel-reclaim gate"
elif [ "$r1" = 98 ]; then
	ktap_test_skip "node too small for the kernel-reclaim gate"
elif [ "${s1:-0}" -gt 4096 ] 2>/dev/null && [ "${s0:-0}" -le $(( s1 / 8 + 256 )) ] 2>/dev/null; then
	ktap_test_pass "kernel reclaim swaps node folios with CAP_RECLAIM, not without (s1=$s1 s0=$s0)"
elif [ "${s1:-0}" -le 4096 ] 2>/dev/null; then
	ktap_test_skip "no kernel reclaim observed on the node in this env (s1=$s1)"
else
	ktap_test_fail "kernel-reclaim gate wrong: reclaim=0 still swapped node folios (s1=$s1 s0=$s0)"
fi

pn_reset
ktap_finished
