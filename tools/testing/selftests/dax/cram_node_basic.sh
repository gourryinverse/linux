#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM basic tests driven through the explicit cramdax test provider and the
# standard reclaim and node accounting interfaces.
#
# Covers, with writeback intentionally NOT engaged:
#   - entry/exit: online registers a private node; offline removes it
#   - ballooning: inflate/deflate reserves/releases private-node capacity
#   - allocation cutoff: inflation caps at the node's free capacity
#
# Provision a dax device on a memoryless node via the memmap= boot param, e.g.
#   memmap=1G!0x180000000      (see vng.workflow)
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

ktap_print_header
pn_require_root

set_balloon() { echo "$1" > "$D/balloon_target" 2>/dev/null; }
wait_balloon()
{
	local op=$1 value=$2 i current

	for ((i = 0; i < 200; i++)); do
		current=$(pn_node_vmstat "$PN" nr_balloon_pages)
		[ -n "$current" ] && [ "$current" "$op" "$value" ] && return 0
		sleep 0.1
	done
	return 1
}

wait_balloon_stable()
{
	local previous=-1 stable=0 current i

	for ((i = 0; i < 300; i++)); do
		current=$(pn_node_vmstat "$PN" nr_balloon_pages)
		if [ "$current" = "$previous" ]; then
			stable=$((stable + 1))
			[ "$stable" -ge 10 ] && return 0
		else
			stable=0
			previous=$current
		fi
		sleep 0.1
	done
	return 1
}

# cram_provision -- bind a memoryless dax device to cramdax and leave it
# offline.  Sets DAX, D (sysfs dir) and PN (target node).  SKIPs otherwise.
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
			{ ktap_skip_all "$DAX has no CRAM state control (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
		echo offline > "$D/state" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node (see header)"
	exit "$KSFT_SKIP"
}

cram_provision
ktap_print_msg "using $DAX on memoryless node $PN"
ktap_set_plan 10

# Make sure we leave the node torn down even if a test aborts.
cleanup() { echo offline > "$D/state" 2>/dev/null; }
trap cleanup EXIT

# 1. feature configuration is accepted while memory is offline
echo 0xe > "$D/memory_features" 2>/dev/null; rc=$?
features=$(cat "$D/memory_features")
echo 0x4 > "$D/memory_features" 2>/dev/null
if [ "$rc" = 0 ] && [ "$features" = 0xe ]; then
	ktap_test_pass "cramdax accepted reclaim+compaction features while offline"
else
	ktap_test_fail "cramdax rejected feature configuration (rc=$rc value=$features)"
fi

# 2. entry: online registers private memory managed by CRAM
echo online > "$D/state" 2>/dev/null; rc=$?
present=$(pn_node_present_pages "$PN")
if [ "$rc" = 0 ] && pn_node_is_private "$PN" &&
   [ -n "$present" ] && [ "$present" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "online: node $PN is private + cram-managed (present=$present)"
else
	ktap_test_fail "online failed: rc=$rc private=$(pn_is_private && echo y || echo n) present=$present"
fi

# 3. memory features are immutable while the node is online
echo 0xe > "$D/memory_features" 2>/dev/null; rc=$?
if [ "$rc" != 0 ] && [ "$(cat "$D/state")" = online ]; then
	ktap_test_pass "memory feature change rejected while online"
else
	ktap_test_fail "memory feature change not rejected while online (rc=$rc)"
fi

# Balloon count is exact; free has per-cpu accounting drift, so the balloon
# column is the primary signal and free is only checked for direction.

# 4. ballooning: inflate reserves private-node pages
N=8192
f0=$(pn_node_vmstat "$PN" nr_free_pages)
b0=$(pn_node_vmstat "$PN" nr_balloon_pages)
set_balloon "$N"
wait_balloon -ge $((b0 + N - 256))
f1=$(pn_node_vmstat "$PN" nr_free_pages)
b1=$(pn_node_vmstat "$PN" nr_balloon_pages)
if [ "$b1" -ge $((b0 + N - 256)) ] 2>/dev/null && [ "$f1" -lt "$f0" ] 2>/dev/null; then
	ktap_test_pass "inflate $N: balloon $b0->$b1, free $f0->$f1"
else
	ktap_test_fail "inflate did not reserve: balloon $b0->$b1, free $f0->$f1"
fi

# 5. ballooning: deflate releases them
set_balloon 0
wait_balloon -le "$b0"
f2=$(pn_node_vmstat "$PN" nr_free_pages)
b2=$(pn_node_vmstat "$PN" nr_balloon_pages)
if [ "$b2" -le "$b0" ] 2>/dev/null && [ "$f2" -gt "$f1" ] 2>/dev/null; then
	ktap_test_pass "deflate $N: balloon $b1->$b2, free $f1->$f2"
else
	ktap_test_fail "deflate did not release: balloon $b1->$b2, free $f1->$f2"
fi

# 6. allocation cutoff: inflating past capacity must NOT exceed the node's own
# pages (no spill onto DRAM) and must substantially fill it.
fbase=$(pn_node_vmstat "$PN" nr_free_pages)
set_balloon "$((present + 65536))"
wait_balloon_stable
fcut=$(pn_node_vmstat "$PN" nr_free_pages)
bcut=$(pn_node_vmstat "$PN" nr_balloon_pages)
if [ "$bcut" -le "$present" ] 2>/dev/null &&
   [ "$bcut" -ge $((present / 2)) ] 2>/dev/null &&
   [ "$fcut" -lt "$fbase" ] 2>/dev/null; then
	ktap_test_pass "cutoff: balloon $bcut <= present $present (no DRAM spill), free $fbase->$fcut"
else
	ktap_test_fail "cutoff wrong: balloon=$bcut present=$present free $fbase->$fcut"
fi

# 7. cutoff is stable: a further inflate at capacity adds (almost) nothing
set_balloon "$((present + 65536))"
wait_balloon_stable
bcut2=$(pn_node_vmstat "$PN" nr_balloon_pages)
if [ "$bcut2" -le $((bcut + 256)) ] 2>/dev/null; then
	ktap_test_pass "cutoff stable: further inflate added $((bcut2 - bcut)) pages"
else
	ktap_test_fail "cutoff not stable: balloon $bcut->$bcut2"
fi

# 8. deflate-all restores capacity (no leak)
set_balloon 0
wait_balloon -le "$b0"
frest=$(pn_node_vmstat "$PN" nr_free_pages)
brest=$(pn_node_vmstat "$PN" nr_balloon_pages)
if [ "$brest" = 0 ] && [ "$frest" -ge $((fbase - fbase / 8)) ] 2>/dev/null; then
	ktap_test_pass "deflate-all restored: balloon=0, free $fcut->$frest (base $fbase)"
else
	ktap_test_fail "deflate-all leaked: balloon=$brest free $fcut->$frest (base $fbase)"
fi

# 9. exit: unplug unregisters + removes the node
echo offline > "$D/state" 2>/dev/null; rc=$?
if [ "$rc" = 0 ] && ! pn_node_is_private "$PN" &&
   ! node_in_mask "$PN" has_memory; then
	ktap_test_pass "unplug: node $PN unregistered + removed"
else
	ktap_test_fail "unplug failed: rc=$rc private=$(pn_is_private && echo y || echo n)"
fi

# 10. entry/exit cycle 3x re-registers cleanly each time (token/np lifetime)
cycle_ok=1; fail_i=0
for i in 1 2 3; do
	if ! echo online > "$D/state" 2>/dev/null ||
	   ! pn_node_is_private "$PN"; then
		cycle_ok=0; fail_i=$i; break
	fi
	if ! echo offline > "$D/state" 2>/dev/null ||
	   pn_node_is_private "$PN"; then
		cycle_ok=0; fail_i=$i; break
	fi
done
if [ "$cycle_ok" = 1 ]; then
	ktap_test_pass "online/unplug cycle 3x: clean register/unregister each time"
else
	ktap_test_fail "online/unplug cycle regressed at iteration $fail_i"
fi

ktap_finished
