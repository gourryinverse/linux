#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node set_mempolicy/home_node placement under CAP_USER_NUMA.
# Nodemask placement is gated
# home_node is NOT (only a preferred-nid hint, does not grant MPOL_F_PRIVATE).
#
#      {P}=Private Bind
#   1. set_mempolicy(MPOL_BIND,{P}) on an opted node is accepted (gate only).
#   2. set_mempolicy_home_node() accepts an opted private node as home.
#   3. process-wide MPOL_BIND to a movable-only private node falls back.
#   4. opt-in cleared: set_mempolicy({P}) is trimmed -> EINVAL.
#   5. ...but home_node({P}) is still accepted (not gated) and falls back off it.
#
# Needs a private node on a memoryless node; see private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision
DRAM0=$(awk -F, '{print $1}' "$NODE_BASE/has_memory"); DRAM0=${DRAM0%%-*}

up() {	# bring PN up with user_numa opt-in = $1
	pn_reset
	pn_set user_numa "$1"
	pn_hotplug online_movable
}

up 1
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN (user_numa) as private"
	pn_reset; exit "$KSFT_SKIP"
fi
ktap_print_msg "using $DAX on private node $PN, dram0=$DRAM0"
ktap_set_plan 5

# 1. gate only: a process-wide strict bind to a movable private node would
#    wedge unmovable (page-table) allocs, so don't drive placement here.
out=$("$TOOL" setmempol "$PN" 16 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "rc=0"; then
	ktap_test_pass "set_mempolicy(MPOL_BIND,{$PN}) accepted for opted private node"
else
	ktap_test_fail "set_mempolicy({$PN}) rejected despite opt-in ($out)"
fi

# 2. bound to DRAM, so pages land there; home_node($PN) just must be accepted
out=$("$TOOL" sethome "$PN" "$DRAM0" 16 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "rc=0" && echo "$out" | grep -q "on_home$PN=0"; then
	ktap_test_pass "set_mempolicy_home_node($PN) accepted for opted private node"
else
	ktap_test_fail "home_node($PN) rejected/misplaced despite opt-in ($out)"
fi

# 3. unmovable allocs (page tables) have no usable zone on a movable-only node
#    and must fall back, not VM_FAULT_OOM-retry forever.  Watchdog -> FAIL not hang.
"$TOOL" bindfault "$PN" 16 >/tmp/pn_bf.$$ 2>&1 & bfp=$!
for _ in $(seq 1 15); do sleep 1; kill -0 "$bfp" 2>/dev/null || break; done
if kill -0 "$bfp" 2>/dev/null; then
	kill -9 "$bfp" 2>/dev/null
	ktap_test_fail "process-wide MPOL_BIND({$PN}) livelocked (unmovable alloc not falling back)"
elif grep -q "bindfault: done" /tmp/pn_bf.$$; then
	sed 's/^/# /' /tmp/pn_bf.$$
	ktap_test_pass "process-wide MPOL_BIND({$PN}) completed; unmovable allocs fell back to real memory"
else
	ktap_test_skip "bindfault inconclusive ($(tr '\n' ';' </tmp/pn_bf.$$))"
fi
rm -f /tmp/pn_bf.$$

# clear the opt-in and re-online
up 0
if ! pn_is_private; then
	ktap_test_skip "node $PN did not re-online private after clearing opt-in"
	ktap_test_skip "(non-opted home-node check skipped)"
	pn_reset; exit 0
fi

# 4.
out=$("$TOOL" setmempol "$PN" 16 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "errno=22"; then
	ktap_test_pass "set_mempolicy({$PN}) on a non-opted private node rejected EINVAL"
else
	ktap_test_fail "set_mempolicy({$PN}) non-opted not rejected ($out)"
fi

# 5. home_node is not cap-gated: accepted even non-opted, and falls back off it.
out=$("$TOOL" sethome "$PN" "$DRAM0" 16 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "rc=0" && echo "$out" | grep -q "on_home$PN=0"; then
	ktap_test_pass "set_mempolicy_home_node($PN) accepted for non-opted private node; fell back off it"
else
	ktap_test_fail "home_node($PN) non-opted: expected accept + fallback off node ($out)"
fi

pn_reset
ktap_finished
