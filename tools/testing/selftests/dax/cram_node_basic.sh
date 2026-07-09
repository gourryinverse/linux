#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM driverless basic tests.  CRAM has no in-tree driver yet, so this drives
# it through the dax/kmem [TEST] 'cram' knob (cram owns the node hotplug) and
# the cram debugfs control/inspection interface.
#
# Covers, with writeback intentionally NOT engaged:
#   - entry/exit: online via the cram knob registers an N_MEMORY_PRIVATE node;
#     unplug unregisters and removes it
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

CRAM_DBG=/sys/kernel/debug/cram

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -d "$CRAM_DBG" ] ||
	{ ktap_skip_all "cram debugfs missing (need CONFIG_CRAM=y)"; exit "$KSFT_SKIP"; }

# cn_field NID COL -- read column COL (a header name) for node NID from
# $CRAM_DBG/nodes.  Empty if the node is absent.
cn_field() {
	awk -v n="$1" -v c="$2" '
		NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
		$1 == n { print $(col[c]); found = 1 }
		END { if (!found) print "" }' "$CRAM_DBG/nodes"
}
ctl() { echo "$1" > "$CRAM_DBG/control" 2>/dev/null; }	# rc = write status

# cram_provision -- find a memoryless dax device, bind it to kmem, and leave it
# unplugged.  Sets DAX, D (sysfs dir) and PN (target node).  SKIPs otherwise.
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
		node_in_mask "$nid" has_memory && continue	# memoryless only
		drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
		[ "$drv" = device_dax ] &&
			basename "$d" > /sys/bus/dax/drivers/device_dax/unbind 2>/dev/null
		[ "$drv" = kmem ] ||
			basename "$d" > /sys/bus/dax/drivers/kmem/new_id 2>/dev/null
		sleep 1
		D=$d; DAX=$(basename "$d"); PN=$nid
		[ -e "$D/cram" ] ||
			{ ktap_skip_all "$DAX has no 'cram' knob (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node (see header)"
	exit "$KSFT_SKIP"
}

cram_provision
ktap_print_msg "using $DAX on memoryless node $PN"
ktap_set_plan 10

# Make sure we leave the node torn down even if a test aborts.
cleanup() { echo unplugged > "$D/state" 2>/dev/null; echo 0 > "$D/cram" 2>/dev/null; }
trap cleanup EXIT

# 1. cram knob accepted while unplugged
echo 1 > "$D/cram" 2>/dev/null; rc=$?
if [ "$rc" = 0 ] && [ "$(cat "$D/cram")" = 1 ]; then
	ktap_test_pass "cram=1 accepted while unplugged"
else
	ktap_test_fail "cram=1 rejected while unplugged (rc=$rc val=$(cat "$D/cram"))"
fi

# 2. entry: online registers an N_MEMORY_PRIVATE node managed by cram
echo online > "$D/state" 2>/dev/null; rc=$?
present=$(cn_field "$PN" present)
if [ "$rc" = 0 ] && node_in_mask "$PN" has_private_memory &&
   [ -n "$present" ] && [ "$present" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "online: node $PN is private + cram-managed (present=$present)"
else
	ktap_test_fail "online failed: rc=$rc private=$(pn_is_private && echo y || echo n) present=$present"
fi

# 3. cram knob rejected while online (state != unplugged)
echo 0 > "$D/cram" 2>/dev/null; rc=$?
if [ "$rc" != 0 ] && [ "$(cat "$D/cram")" = 1 ]; then
	ktap_test_pass "cram toggle rejected while online"
else
	ktap_test_fail "cram toggle not rejected while online (rc=$rc val=$(cat "$D/cram"))"
fi

# Balloon count is exact; free has per-cpu accounting drift, so the balloon
# column is the primary signal and free is only checked for direction.

# 4. ballooning: inflate reserves private-node pages
N=8192
f0=$(cn_field "$PN" free); b0=$(cn_field "$PN" balloon)
ctl "inflate $PN $N"
f1=$(cn_field "$PN" free); b1=$(cn_field "$PN" balloon)
if [ "$b1" -ge $((b0 + N - 256)) ] 2>/dev/null && [ "$f1" -lt "$f0" ] 2>/dev/null; then
	ktap_test_pass "inflate $N: balloon $b0->$b1, free $f0->$f1"
else
	ktap_test_fail "inflate did not reserve: balloon $b0->$b1, free $f0->$f1"
fi

# 5. ballooning: deflate releases them
ctl "deflate $PN $N"
f2=$(cn_field "$PN" free); b2=$(cn_field "$PN" balloon)
if [ "$b2" -le "$b0" ] 2>/dev/null && [ "$f2" -gt "$f1" ] 2>/dev/null; then
	ktap_test_pass "deflate $N: balloon $b1->$b2, free $f1->$f2"
else
	ktap_test_fail "deflate did not release: balloon $b1->$b2, free $f1->$f2"
fi

# 6. allocation cutoff: inflating past capacity must NOT exceed the node's own
# pages (no spill onto DRAM) and must substantially fill it.
fbase=$(cn_field "$PN" free)
ctl "inflate $PN $((present + 65536))"
fcut=$(cn_field "$PN" free); bcut=$(cn_field "$PN" balloon)
if [ "$bcut" -le "$present" ] 2>/dev/null && [ "$bcut" -ge $((present / 2)) ] 2>/dev/null &&
   [ "$fcut" -lt $((fbase / 4)) ] 2>/dev/null; then
	ktap_test_pass "cutoff: balloon $bcut <= present $present (no DRAM spill), free $fbase->$fcut"
else
	ktap_test_fail "cutoff wrong: balloon=$bcut present=$present free $fbase->$fcut"
fi

# 7. cutoff is stable: a further inflate at capacity adds (almost) nothing
ctl "inflate $PN 65536"
bcut2=$(cn_field "$PN" balloon)
if [ "$bcut2" -le $((bcut + 256)) ] 2>/dev/null; then
	ktap_test_pass "cutoff stable: further inflate added $((bcut2 - bcut)) pages"
else
	ktap_test_fail "cutoff not stable: balloon $bcut->$bcut2"
fi

# 8. deflate-all restores capacity (no leak)
ctl "deflate $PN $((present * 2))"
frest=$(cn_field "$PN" free); brest=$(cn_field "$PN" balloon)
if [ "$brest" = 0 ] && [ "$frest" -ge $((fbase - fbase / 8)) ] 2>/dev/null; then
	ktap_test_pass "deflate-all restored: balloon=0, free $fcut->$frest (base $fbase)"
else
	ktap_test_fail "deflate-all leaked: balloon=$brest free $fcut->$frest (base $fbase)"
fi

# 9. exit: unplug unregisters + removes the node
echo unplugged > "$D/state" 2>/dev/null; rc=$?
if [ "$rc" = 0 ] && ! node_in_mask "$PN" has_private_memory &&
   [ -z "$(cn_field "$PN" present)" ]; then
	ktap_test_pass "unplug: node $PN unregistered + removed"
else
	ktap_test_fail "unplug failed: rc=$rc private=$(pn_is_private && echo y || echo n) row='$(cn_field "$PN" present)'"
fi

# 10. entry/exit cycle 3x re-registers cleanly each time (token/np lifetime)
cycle_ok=1; fail_i=0
for i in 1 2 3; do
	if ! echo online > "$D/state" 2>/dev/null ||
	   ! node_in_mask "$PN" has_private_memory; then
		cycle_ok=0; fail_i=$i; break
	fi
	if ! echo unplugged > "$D/state" 2>/dev/null ||
	   node_in_mask "$PN" has_private_memory; then
		cycle_ok=0; fail_i=$i; break
	fi
done
if [ "$cycle_ok" = 1 ]; then
	ktap_test_pass "online/unplug cycle 3x: clean register/unregister each time"
else
	ktap_test_fail "online/unplug cycle regressed at iteration $fail_i"
fi

ktap_finished
