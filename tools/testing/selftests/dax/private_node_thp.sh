#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node THP collapse -- both MADV_COLLAPSE (user-initiated) and khugepaged
# (kernel daemon) -- is gated on CAP_COLLAPSE (N_MEMORY_COLLAPSE).  With the cap
# the node participates in collapse; without it neither may form an on-node THP.
# (Placing the base pages via mbind needs CAP_USER_NUMA.)
#
#   1. MADV_COLLAPSE with CAP_COLLAPSE: on-node THP forms.
#   2. khugepaged with CAP_COLLAPSE: on-node THP forms.
#   3. MADV_COLLAPSE WITHOUT CAP_COLLAPSE: no on-node THP (gate blocks it).
#   4. khugepaged WITHOUT CAP_COLLAPSE: no on-node THP (gate blocks it).
#
# Needs a private node on a memoryless node + THP; see private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

THP=/sys/kernel/mm/transparent_hugepage

# AnonHugePages (kB) from the tool's "post on_nodeN=... AnonHugePages=...kB" line.
ahp_of() { echo "$1" | sed -n 's/.*post on_node'"$2"'=[0-9]*.*AnonHugePages=\([0-9]*\)kB/\1/p'; }

pn_begin
pn_require_tool
pn_provision
[ -e "$CAPF" ] || { ktap_skip_all "$DAX has no capability knob"; exit "$KSFT_SKIP"; }
[ -d "$THP" ] || { ktap_skip_all "THP unsupported (CONFIG_TRANSPARENT_HUGEPAGE)"; exit "$KSFT_SKIP"; }

# THP=madvise + a brisk khugepaged so the daemon-driven case resolves quickly.
# Save + restore the tunables so this test does not leave khugepaged spinning
# aggressively into subsequent tests (which can wedge their hot-unplugs).
SAVE_EN=$(sed -n 's/.*\[\(.*\)\].*/\1/p' "$THP/enabled" 2>/dev/null)
SAVE_SS=$(cat "$THP/khugepaged/scan_sleep_millisecs" 2>/dev/null)
SAVE_AS=$(cat "$THP/khugepaged/alloc_sleep_millisecs" 2>/dev/null)
restore_thp() {
	[ -n "$SAVE_EN" ] && echo "$SAVE_EN" > "$THP/enabled" 2>/dev/null
	[ -n "$SAVE_SS" ] && echo "$SAVE_SS" > "$THP/khugepaged/scan_sleep_millisecs" 2>/dev/null
	[ -n "$SAVE_AS" ] && echo "$SAVE_AS" > "$THP/khugepaged/alloc_sleep_millisecs" 2>/dev/null
}
trap 'restore_thp' EXIT
echo madvise > "$THP/enabled" 2>/dev/null
echo 100 > "$THP/khugepaged/scan_sleep_millisecs" 2>/dev/null
echo 0   > "$THP/khugepaged/alloc_sleep_millisecs" 2>/dev/null

up() {	# user_numa=1 (mbind base pages), collapse=$1 (MADV/khugepaged), reclaim=1 (huge alloc)
	pn_reset
	pn_set user_numa 1
	pn_set collapse "${1:-1}"
	pn_set reclaim 1
	pn_hotplug online_movable
}

ktap_set_plan 4

up
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "node $PN did not online private (1)"
	ktap_test_skip "node $PN did not online private (2)"
	pn_reset; exit "$KSFT_SKIP"
fi

# 1. MADV_COLLAPSE is gated on CAP_COLLAPSE: forms an on-node THP.
out=$("$TOOL" collapse "$PN" 4 2>&1); echo "$out" | sed 's/^/# /'
if [ "$(ahp_of "$out" "$PN")" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "MADV_COLLAPSE formed an on-node THP via CAP_COLLAPSE"
else
	ktap_test_fail "MADV_COLLAPSE did not form an on-node THP with collapse set ($out)"
fi

# 2. khugepaged collapses private-node folios when CAP_COLLAPSE is granted.
out=$("$TOOL" khugecollapse "$PN" 4 20 2>&1); echo "$out" | sed 's/^/# /'
if [ "$(ahp_of "$out" "$PN")" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "khugepaged collapsed onto private node $PN via CAP_COLLAPSE"
else
	ktap_test_fail "khugepaged did not collapse onto private node $PN with collapse set ($out)"
fi

# Re-online WITHOUT CAP_COLLAPSE (keep user_numa so the base pages still land
# on-node); collapse must now be gated for both the userspace and daemon paths.
up 0
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "node $PN did not online private for the negative gate (3)"
	ktap_test_skip "node $PN did not online private for the negative gate (4)"
	pn_reset; ktap_finished; exit 0
fi

# 3. MADV_COLLAPSE without CAP_COLLAPSE: no on-node THP.
out=$("$TOOL" collapse "$PN" 4 2>&1); echo "$out" | sed 's/^/# /'
if [ "$(ahp_of "$out" "$PN")" -eq 0 ] 2>/dev/null; then
	ktap_test_pass "MADV_COLLAPSE formed no on-node THP without CAP_COLLAPSE (gated)"
else
	ktap_test_fail "MADV_COLLAPSE formed an on-node THP despite collapse cleared ($out)"
fi

# 4. khugepaged without CAP_COLLAPSE: no on-node THP.
out=$("$TOOL" khugecollapse "$PN" 4 20 2>&1); echo "$out" | sed 's/^/# /'
if [ "$(ahp_of "$out" "$PN")" -eq 0 ] 2>/dev/null; then
	ktap_test_pass "khugepaged formed no on-node THP without CAP_COLLAPSE (gated)"
else
	ktap_test_fail "khugepaged collapsed onto $PN despite collapse cleared ($out)"
fi

pn_reset
ktap_finished
