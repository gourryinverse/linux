#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# migrate_pages(2) (node-to-node) to/from a private node, gated by user_numa.
#
#   G1 off an opted private node -> migrates to DRAM.
#   G2 ONTO an opted private node -> migrates (symmetric with move_pages):
#      migrate_to_node() routes the target allocation through the node's
#      private zonelist, and kernel_migrate_pages() admits the opted-in node.
#
# Needs a private node on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision
pn_reset
DRAM=$(pn_dram_list); set -- $DRAM; D0=$1
ktap_print_msg "private node $PN; DRAM={$DRAM} D0=$D0"
ktap_set_plan 2

up() {	# online PN opted into user_numa
	pn_reset
	pn_set user_numa 1
	pn_set hotunplug 1
	pn_hotplug online_movable
}

# G1: off an opted private node -> migrates to DRAM
up
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "G1 could not online $PN opted"
	ktap_test_skip "G2 (depends on opted online)"
else
	out=$("$TOOL" migratepages2 "$PN" "$D0" 16 2>&1); echo "$out" | sed 's/^/# /'
	on_old=$(echo "$out" | pn_field "on_old$PN")
	on_new=$(echo "$out" | pn_field "on_new$D0")
	if [ "${on_old:-1}" = 0 ] && [ "${on_new:-0}" -gt 0 ] 2>/dev/null; then
		ktap_test_pass "G1 migrate_pages off opted private $PN -> $D0 moved all (on_old=0 on_new=$on_new)"
	else
		ktap_test_fail "G1 expected drain off $PN (on_old=$on_old on_new=$on_new)"
	fi

	# G2: ONTO an opted private node -> migrates (private zonelist), symmetric with move_pages
	out=$("$TOOL" migratepages2 "$D0" "$PN" 16 2>&1); echo "$out" | sed 's/^/# /'
	on_old=$(echo "$out" | pn_field "on_old$D0")
	on_new=$(echo "$out" | pn_field "on_new$PN")
	if [ "${on_old:-1}" = 0 ] && [ "${on_new:-0}" -gt 0 ] 2>/dev/null; then
		ktap_test_pass "G2 migrate_pages ONTO opted private $PN moved all (on_old=0 on_new=$on_new) -- symmetric with move_pages"
	else
		ktap_test_fail "G2 expected migration onto opted private $PN (on_old=$on_old on_new=$on_new)"
	fi
fi

pn_reset
ktap_finished
