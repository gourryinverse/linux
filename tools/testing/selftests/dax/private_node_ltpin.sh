#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node NODE_PRIVATE_CAP_LTPIN test.
#
#   1. opted OUT: longterm pin is rejected and the folio is not migrated off
#   2. opted IN: longterm pin succeeds like ordinary memory
#   3. opted IN: a held longterm pin blocks hot-unplug (the pinned folio is
#      unmigratable, so offline is refused); releasing the pin lets it proceed.
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
	ktap_skip_all "$DAX has no user_numa opt-in (NODE_PRIVATE_CAP_USER_NUMA)"
	exit "$KSFT_SKIP"
fi
pn_reset
ktap_set_plan 3

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

# 3. a held longterm pin blocks hot-unplug; releasing it lets the unplug proceed.
# Online ZONE_NORMAL (online_kernel) so the longterm pin stays in place on the
# node (a ZONE_MOVABLE pin would migrate the folio off-node before pinning).
pn_reset
pn_set user_numa 1
pn_set ltpin 1
pn_set hotunplug 1
pn_hotplug online_kernel
HF=/tmp/pn_ltpin_h.$$
if [ "$(pn_state)" = online_kernel ] && pn_is_private; then
	: > "$HF"
	"$TOOL" ltpinhold "/dev/$DAX" 8 "$PN" 120 >"$HF" 2>&1 &
	HJOB=$!
	for _ in $(seq 1 50); do grep -q "pid=" "$HF" && break; sleep 0.2; done
	held=$(sed -n 's/.*pinned=\([a-z]*\).*/\1/p' "$HF" | head -1)
	if [ "$held" != yes ]; then
		ktap_test_skip "could not establish a held longterm pin ($(tr '\n' ';' <"$HF"))"
	else
		pn_hotplug unplugged; rc_pin=$?; st_pin=$(pn_state)
		kill "$HJOB" 2>/dev/null; wait "$HJOB" 2>/dev/null	# fd close drops the pin
		sleep 1
		pn_hotplug unplugged; rc_free=$?; st_free=$(pn_state)
		ktap_print_msg "pin-vs-unplug: pinned(rc=$rc_pin st=$st_pin) released(rc=$rc_free st=$st_free)"
		if [ "$rc_pin" != 0 ] && [ "$st_pin" = online_kernel ] &&
		   [ "$rc_free" = 0 ] && [ "$st_free" = unplugged ]; then
			ktap_test_pass "longterm pin blocked hot-unplug; unplug succeeded once released"
		else
			ktap_test_fail "pin/unplug contract wrong (pinned rc=$rc_pin/$st_pin, released rc=$rc_free/$st_free)"
		fi
	fi
	kill -9 "$HJOB" 2>/dev/null; wait "$HJOB" 2>/dev/null
else
	ktap_test_skip "could not online $PN private (kernel zone) for the pin-vs-unplug check"
fi
rm -f "$HF"

pn_reset
ktap_finished
