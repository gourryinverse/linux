#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node mbind() placement under CAP_USER_NUMA.
# {P}=Private bind    {D}=DRAM bind
#
#   1. mbind({P1}) onto an opted private node places all pages there.
#   2. mbind({P1,P2}) honored; pages stay on private nodes (none on DRAM). (needs >=2)
#   3. mbind({D0,P1}) honored; pages stay within the mask, none leak.
#   4. mbind({P1}) on a non-opted private node is trimmed -> empty mask -> EINVAL.
#
# Needs private node(s) on memoryless node(s); see private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision_all
set -- $PN_NODES
nprivate=$#
[ "$nprivate" -ge 1 ] || { ktap_skip_all "no private node provisioned"; exit "$KSFT_SKIP"; }

# opt every private node into user_numa placement + hot-unplug
for d in $PN_DAXES; do
	echo unplugged > "$DAX_BASE/$d/state" 2>/dev/null
	echo 1 > "$DAX_BASE/$d/user_numa" 2>/dev/null
	echo 1 > "$DAX_BASE/$d/hotunplug" 2>/dev/null
	echo online_movable > "$DAX_BASE/$d/state" 2>/dev/null
done
sleep 1
P1=$(echo $PN_NODES | awk '{print $1}')
P2=$(echo $PN_NODES | awk '{print $2}')
D1=$(echo $PN_DAXES | awk '{print $1}')
DRAM0=$(awk -F, '{print $1}' "$NODE_BASE/has_memory"); DRAM0=${DRAM0%%-*}
node_in_mask "$P1" has_private_memory || { ktap_skip_all "node $P1 did not online as private"; exit "$KSFT_SKIP"; }
ktap_print_msg "private nodes={$PN_NODES} D0=$DRAM0"
ktap_set_plan 4

# 1.
out=$("$TOOL" mbind "$P1" 16 2>&1); echo "$out" | sed 's/^/# /'
placed=$(echo "$out" | pn_field "on_node$P1")
total=$(echo "$out" | pn_field total)
if echo "$out" | grep -q "rc=0" && [ "${placed:-0}" -gt 0 ] && [ "$placed" = "$total" ] 2>/dev/null; then
	ktap_test_pass "mbind({$P1}) placed all $placed pages on the private node"
else
	ktap_test_fail "mbind({$P1}) did not place on the private node ($out)"
fi

# 2.
if [ "$nprivate" -lt 2 ]; then
	ktap_test_skip "need >=2 private nodes for the multi-private mask check"
else
	out=$("$TOOL" mbindmask 16 "$P1" "$P2" 2>&1); echo "$out" | sed 's/^/# /'
	on1=$(echo "$out" | pn_field "on_node$P1")
	total=$(echo "$out" | pn_field total)
	if echo "$out" | grep -q "rc=0" && [ "${on1:-0}" -gt 0 ] && [ "$on1" = "$total" ] 2>/dev/null; then
		ktap_test_pass "mbind({$P1,$P2}) honored; all $total pages on private nodes (none on DRAM)"
	else
		ktap_test_fail "mbind({$P1,$P2}) not honored or leaked off private ($out)"
	fi
fi

# 3.
out=$("$TOOL" mbindmask 16 "$DRAM0" "$P1" 2>&1); echo "$out" | sed 's/^/# /'
on0=$(echo "$out" | pn_field "on_node$DRAM0")
total=$(echo "$out" | pn_field total)
if echo "$out" | grep -q "rc=0" && [ "${on0:-0}" -gt 0 ] && [ "$on0" = "$total" ] 2>/dev/null; then
	ktap_test_pass "mbind({$DRAM0,$P1}) honored; all $total pages within the mask (on DRAM)"
else
	ktap_test_fail "mbind({$DRAM0,$P1}) not honored or leaked ($out)"
fi

# 4. clear the opt-in: the node is trimmed -> empty mask -> EINVAL
echo unplugged > "$DAX_BASE/$D1/state" 2>/dev/null
echo 0 > "$DAX_BASE/$D1/user_numa" 2>/dev/null
echo online_movable > "$DAX_BASE/$D1/state" 2>/dev/null
out=$("$TOOL" mbind "$P1" 16 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "errno=22"; then
	ktap_test_pass "mbind({$P1}) on a non-opted private node rejected EINVAL (trimmed)"
else
	ktap_test_fail "mbind({$P1}) non-opted not rejected ($out)"
fi

for d in $PN_DAXES; do echo unplugged > "$DAX_BASE/$d/state" 2>/dev/null; done
ktap_finished
