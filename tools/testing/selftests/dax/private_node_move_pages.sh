#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# move_pages(2) to/from a private node, gated by the user_numa opt-in.
#
#   1. source: opted private folio migrates off to DRAM.
#   2. target: DRAM page placed onto the opted private node.
#   3. source: non-opted private folio rejected (-ENOENT).
#   4. target: non-opted private node rejected (-ENODEV).
#
# Needs a kmem-private dax device on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision
DRAM0=$(awk -F, '{print $1}' "$NODE_BASE/has_memory"); DRAM0=${DRAM0%%-*}

up() {	# online PN with user_numa opt-in = $1
	pn_reset
	pn_set hotunplug 1
	pn_set user_numa "$1"
	pn_hotplug online_movable
}

up 1
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN (user_numa) as private"
	pn_reset; exit "$KSFT_SKIP"
fi
ktap_print_msg "using $DAX on private node $PN, dram0=$DRAM0"
ktap_set_plan 4

# 1. source: opted private folio migrates off to DRAM
out=$("$TOOL" movepages "/dev/$DAX" "$DRAM0" 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "status=$DRAM0"; then
	ktap_test_pass "move_pages({$PN} folio -> $DRAM0) migrated an opted private folio off"
else
	ktap_test_fail "move_pages of an opted private folio did not migrate ($out)"
fi

# 2. target: DRAM page placed onto the opted private node
out=$("$TOOL" movepagesto "$PN" 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "now on node$PN"; then
	ktap_test_pass "move_pages(DRAM page -> {$PN}) placed it on the opted private node"
else
	ktap_test_fail "move_pages onto opted private node $PN did not place it there ($out)"
fi

# clear the opt-in and re-online
up 0
if ! pn_is_private; then
	ktap_test_skip "node $PN did not re-online private after clearing opt-in"
	ktap_test_skip "(non-opted target check skipped)"
	pn_reset; exit 0
fi

# 3. source: non-opted private folio migration is rejected (-ENOENT)
out=$("$TOOL" movepages "/dev/$DAX" "$DRAM0" 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "status=-2"; then
	ktap_test_pass "move_pages of a non-opted private folio rejected (-ENOENT)"
else
	ktap_test_fail "move_pages of non-opted private folio not rejected ($out)"
fi

# 4. target: non-opted private node rejected as a target (-ENODEV)
out=$("$TOOL" movepagesto "$PN" 2>&1); echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "errno=19"; then
	ktap_test_pass "move_pages onto a non-opted private node rejected (-ENODEV)"
else
	ktap_test_fail "move_pages onto non-opted private node not rejected -ENODEV ($out)"
fi

pn_reset
ktap_finished
