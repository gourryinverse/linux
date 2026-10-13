#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Exercise kmem private-mode configuration and memory hotplug.
# DAX_KMEM_TEST_DEV=<daxX.Y|auto> opts into mutating a test device.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

ktap_print_header
pn_require_root
pn_require_test_device

pn_modprobe kmem

trap pn_unbind_kmem_private EXIT

for D in "$DAX_BASE"/dax*; do
	[ "$DAX_KMEM_TEST_DEV" = auto ] ||
		[ "$(basename "$D")" = "$DAX_KMEM_TEST_DEV" ] || continue
	[ -e "$D/target_node" ] || continue
	PN=$(cat "$D/target_node")
	[ "$PN" -ge 0 ] 2>/dev/null || continue
	pn_bind_kmem_private "$D" || continue
	break
done
[ -n "${D:-}" ] && [ "$(cat "$D/private" 2>/dev/null)" = 1 ] &&
pn_is_private || {
	pn_skip_or_fail "no private kmem device"
}

if ! echo unplugged > "$D/state" 2>/dev/null ||
   node_in_mask "$PN" has_memory; then
	ktap_exit_fail_msg "private memory cannot be unplugged"
fi
if ! echo 0 > "$D/private" 2>/dev/null ||
   ! echo 1 > "$D/private" 2>/dev/null; then
	ktap_exit_fail_msg "private mode cannot be selected while unplugged"
fi

ktap_set_plan 10
if [ "$(cat "$D/private")" = 1 ]; then
	ktap_test_pass "private mode can be selected while unplugged"
else
	ktap_test_fail "private setting was not retained"
fi

if ! echo invalid > "$D/private" 2>/dev/null &&
   [ "$(cat "$D/private")" = 1 ]; then
	ktap_test_pass "invalid private setting is rejected"
else
	ktap_test_fail "invalid private setting changed configuration"
fi

if echo online_movable > "$D/state" 2>/dev/null && pn_is_private &&
   ! node_in_mask "$PN" has_common_memory &&
   node_in_mask "$PN" has_mpol_memory; then
	ktap_test_pass "private online publishes private memory that memory policies may target"
else
	ktap_test_fail "private memory was not onlined"
fi

if ! echo 0 > "$D/private" 2>/dev/null &&
   [ "$(cat "$D/private")" = 1 ]; then
	ktap_test_pass "private mode cannot change while memory blocks exist"
else
	ktap_test_fail "private mode changed while online"
fi

if echo 1 > "$D/private" 2>/dev/null; then
	ktap_test_pass "repeating the current private setting is harmless"
else
	ktap_test_fail "repeating the private setting failed"
fi

if echo unplugged > "$D/state" 2>/dev/null &&
   ! node_in_mask "$PN" has_memory &&
   ! node_in_mask "$PN" has_private_memory &&
   ! node_in_mask "$PN" has_mpol_memory; then
	ktap_test_pass "unplug removes the private node from memory lists"
else
	ktap_test_fail "private memory remained after unplug"
fi

if echo 0 > "$D/private" 2>/dev/null &&
   [ "$(cat "$D/private")" = 0 ]; then
	ktap_test_pass "common mode can be selected after unplug"
else
	ktap_test_fail "common mode could not be selected"
fi

if echo online_movable > "$D/state" 2>/dev/null &&
   node_in_mask "$PN" has_common_memory; then
	ktap_test_pass "common online preserves normal kmem behavior"
else
	ktap_test_fail "common memory was not onlined"
fi

if ! echo 1 > "$D/private" 2>/dev/null &&
   [ "$(cat "$D/private")" = 0 ]; then
	ktap_test_pass "online common memory cannot be reclassified as private"
else
	ktap_test_fail "online common memory was reclassified"
fi

if echo unplugged > "$D/state" 2>/dev/null &&
   echo 1 > "$D/private" 2>/dev/null &&
   echo online_movable > "$D/state" 2>/dev/null && pn_is_private; then
	ktap_test_pass "private mode can be restored through an unplug cycle"
else
	ktap_test_fail "private mode could not be restored"
fi

ktap_finished
