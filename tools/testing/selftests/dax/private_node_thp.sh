#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node THP collapse -- both MADV_COLLAPSE (user-initiated) and
# khugepaged (kernel daemon) -- treats private memory like ZONE_DEVICE and
# leaves its folios alone.  Placing the base pages via mbind needs
# FEAT_USER_NUMA.
#
#   1. MADV_COLLAPSE forms no on-node THP.
#   2. khugepaged forms no on-node THP.
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

up() {	# user_numa=1 lets the test place base pages on the private node
	pn_reset
	pn_set user_numa 1
	pn_online_as online
}

ktap_set_plan 2

if ! up; then
	ktap_test_skip "node $PN did not online private (1)"
	ktap_test_skip "node $PN did not online private (2)"
	pn_reset; exit "$KSFT_SKIP"
fi

# 1. MADV_COLLAPSE must not collapse private-node folios.
out=$("$TOOL" collapse "$PN" 4 2>&1); echo "$out" | sed 's/^/# /'
if [ "$(ahp_of "$out" "$PN")" -eq 0 ] 2>/dev/null; then
	ktap_test_pass "MADV_COLLAPSE formed no THP on private node $PN"
else
	ktap_test_fail "MADV_COLLAPSE formed a THP on private node $PN ($out)"
fi

# 2. khugepaged must not collapse private-node folios.
out=$("$TOOL" khugecollapse "$PN" 4 20 2>&1); echo "$out" | sed 's/^/# /'
if [ "$(ahp_of "$out" "$PN")" -eq 0 ] 2>/dev/null; then
	ktap_test_pass "khugepaged formed no THP on private node $PN"
else
	ktap_test_fail "khugepaged formed a THP on private node $PN ($out)"
fi

pn_reset
ktap_finished
