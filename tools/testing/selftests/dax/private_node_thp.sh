#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node THP collapse is gated by the CALLER (it allocates a fresh huge
# folio on the node):
#   - MADV_COLLAPSE (user-initiated) -> CAP_USER_NUMA
#   - khugepaged (kernel daemon) never operates on private-node folios, the
#     same as ZONE_DEVICE, regardless of any capability.
#
#   1. MADV_COLLAPSE with user_numa: on-node THP forms.
#   2. khugepaged: refused, no on-node THP (private is off-limits to the daemon).
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
[ -e "$D/user_numa" ] || { ktap_skip_all "$DAX missing user_numa cap attribute"; exit "$KSFT_SKIP"; }
[ -d "$THP" ] || { ktap_skip_all "THP unsupported (CONFIG_TRANSPARENT_HUGEPAGE)"; exit "$KSFT_SKIP"; }

# THP=madvise + a brisk khugepaged so the daemon-driven case resolves quickly.
echo madvise > "$THP/enabled" 2>/dev/null
echo 100 > "$THP/khugepaged/scan_sleep_millisecs" 2>/dev/null
echo 0   > "$THP/khugepaged/alloc_sleep_millisecs" 2>/dev/null

up() {	# online PN with user_numa=1, reclaim=1 (helps on-node huge alloc)
	pn_reset
	pn_set hotunplug 1
	pn_set user_numa 1
	pn_set reclaim 1
	pn_hotplug online_movable
}

ktap_set_plan 2

up
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "node $PN did not online private (1)"
	ktap_test_skip "node $PN did not online private (2)"
	pn_reset; exit "$KSFT_SKIP"
fi

# 1. MADV_COLLAPSE is gated on CAP_USER_NUMA: forms an on-node THP.
out=$("$TOOL" collapse "$PN" 4 2>&1); echo "$out" | sed 's/^/# /'
if [ "$(ahp_of "$out" "$PN")" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "MADV_COLLAPSE formed an on-node THP via CAP_USER_NUMA"
else
	ktap_test_fail "MADV_COLLAPSE did not form an on-node THP with user_numa set ($out)"
fi

# 2. khugepaged never collapses private-node folios (off-limits like ZONE_DEVICE).
out=$("$TOOL" khugecollapse "$PN" 4 20 2>&1); echo "$out" | sed 's/^/# /'
if [ "$(ahp_of "$out" "$PN")" -eq 0 ] 2>/dev/null; then
	ktap_test_pass "khugepaged did not collapse onto private node $PN (no on-node THP)"
else
	ktap_test_fail "khugepaged collapsed onto private node $PN ($out)"
fi

pn_reset
ktap_finished
