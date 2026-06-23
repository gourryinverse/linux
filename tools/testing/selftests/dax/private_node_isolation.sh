#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node placement test: dax-mapped memory lands on its private node and
# MAP_SHARED is rejected.
#
#   1. faulted pages are resident on the private node
#   2. MAP_SHARED is rejected
#
# Placement comes from the driver stamping an MPOL_F_PRIVATE bind via
# ->get_policy; needs no opt-in.  The node is brought up ZONE_MOVABLE with
# hotunplug=1 so teardown can migrate residue off and unplug never EBUSYs.
# Needs a kmem-bindable dax device on a memoryless node; SKIPs otherwise.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision
pn_reset

pn_set hotunplug 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "$DAX: could not online node $PN as private (state=$(pn_state))"
	pn_reset; exit "$KSFT_SKIP"
fi
ktap_print_msg "using $DAX on private node $PN"
ktap_set_plan 2

# 1. faulted pages are resident on the private node
out=$("$TOOL" map "/dev/$DAX" 64 "$PN"); rc=$?
total=$(echo "$out" | pn_field total_pages)
onnode=$(echo "$out" | pn_field "on_node$PN")
if [ "$rc" = "$KSFT_SKIP" ]; then
	ktap_test_skip "mmap/fault unavailable: $out"
elif [ -n "$total" ] && [ "$total" -gt 0 ] && [ "$onnode" = "$total" ]; then
	ktap_test_pass "all $total faulted pages resident on private node $PN"
else
	ktap_test_fail "off-node placement: on_node$PN=$onnode of total=$total ($out)"
fi

# 2. MAP_SHARED is rejected (private-node mappings are private by definition)
out=$("$TOOL" shared "/dev/$DAX")
if echo "$out" | grep -q 'shared_mmap=rejected'; then
	ktap_test_pass "MAP_SHARED rejected ($out)"
else
	ktap_test_fail "MAP_SHARED not rejected ($out)"
fi

pn_reset
ktap_finished
