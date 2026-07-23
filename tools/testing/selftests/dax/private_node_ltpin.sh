#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node NODE_MEMORY_CAP_LTPIN test.
#
#   1. opted OUT: longterm pin is rejected and the folio is not migrated off
#   2. opted IN: longterm pin succeeds like ordinary memory
#
# A FOLL_LONGTERM pin of a folio on a non-opted-in private node must fail
# outright and leave the folio in place - neither pinnable nor migratable.
#
# Drives the pin via the gup_test debugfs ioctl, so needs CONFIG_GUP_TEST=y and
# debugfs mounted. Also needs a kmem-bindable dax device on a memoryless node.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_need_gup_test
pn_provision
ktap_print_msg "using $DAX on private node $PN"

# ltpin needs CAP_MBIND too, so userspace can place anon memory on the node.
ltpin_run() {	# $1 = ltpin opt-in (0/1) ; echoes the tool's verdict line
	pn_reset
	pn_set user_numa 1
	pn_set ltpin "$1"
	pn_hotplug online_kernel
	"$TOOL" ltpin "/dev/$DAX" 8 "$PN"
	pn_hotplug unplugged
}

if ! pn_set user_numa 1 || [ "$(pn_get user_numa)" != 1 ]; then
	ktap_skip_all "$DAX has no user_numa opt-in (NODE_MEMORY_CAP_USER_NUMA)"
	exit "$KSFT_SKIP"
fi
pn_reset
ktap_set_plan 2

# 1. opted OUT: pin must fail and the folio must not be migrated off the node
out=$(ltpin_run 0)
pinned=$(echo "$out" | pn_field pinned)
total=$(echo "$out" | pn_field total_pages)
onnode=$(echo "$out" | pn_field "on_node$PN")
if [ "$pinned" = no ] && [ -n "$total" ] && [ "$total" -gt 0 ] && [ "$onnode" = "$total" ]; then
	ktap_test_pass "opted-out: longterm pin rejected, folios not migrated ($out)"
else
	ktap_test_fail "opted-out pin not handled correctly ($out)"
fi

# 2. opted IN: pin succeeds
out=$(ltpin_run 1)
pinned=$(echo "$out" | pn_field pinned)
if [ "$pinned" = yes ]; then
	ktap_test_pass "opted-in: longterm pin succeeded ($out)"
else
	ktap_test_fail "opted-in pin did not succeed ($out)"
fi

pn_reset
ktap_finished
