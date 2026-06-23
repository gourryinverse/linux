#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Manual compaction of a private node (/sys/devices/system/node/nodeN/compact)
# is gated by CAP_RECLAIM (node_allows_reclaim) - a private node compacts only
# once it opts into reclaim, since compaction is part of the make-room domain.
#
#   1. reclaim cleared: writing the node's compact attr is rejected (-EINVAL).
#   2. reclaim opted in: writing the node's compact attr is accepted.
#
# Needs a kmem-bindable dax device on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_provision
COMPACT="$NODE_BASE/node$PN/compact"		# exists only while the node is online
ktap_set_plan 2

# 1. reclaim cleared -> the gate rejects manual compaction (-EINVAL)
pn_reset
pn_set reclaim 0
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "could not online $PN private (reclaim=0)"
elif [ ! -e "$COMPACT" ]; then
	ktap_test_skip "no per-node compact attr for $PN (CONFIG_COMPACTION?)"
elif echo 1 > "$COMPACT" 2>/dev/null; then
	ktap_test_fail "compact accepted on a non-reclaim private node $PN"
else
	ktap_test_pass "compact rejected on a non-reclaim private node $PN (-EINVAL)"
fi

# 2. reclaim opted in -> manual compaction is accepted
pn_reset
pn_set reclaim 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "could not online $PN private (reclaim=1)"
elif [ ! -e "$COMPACT" ]; then
	ktap_test_skip "no per-node compact attr for $PN"
elif echo 1 > "$COMPACT" 2>/dev/null; then
	ktap_test_pass "compact accepted on a reclaim-opted private node $PN"
else
	ktap_test_fail "compact rejected despite CAP_RECLAIM on $PN"
fi

pn_reset
ktap_finished
