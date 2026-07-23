#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node hotplug ABI test.
#
#   1. plugging the device in brings the node up private
#   2. unplugging takes its memory away again
#   3. an invalid hotplug state string is rejected
#
# What this deliberately does NOT cover any more: a node's feature mask is
# fixed when the node is provisioned.  There is no per-device setter to write
# one bit at a time, so the rules node_features_register() enforces -- DEMOTION
# requires RECLAIM, and a claim is -EBUSY once the node holds memory -- cannot
# be driven from here at all.
#
# Five subtests used to try.  They asked pn_set() to "write" an opt-in and then
# checked it "read back", but pn_set() SELECTS a node of that class; it has
# never written anything.  So they were asserting against whichever node came
# back, which is why two of them failed once node selection started working and
# the other three passed for no reason.  The rules they meant to cover are
# driven through the provisioning path by private_node_features_claim.sh, which
# loads the provider with the masks it wants to see refused.
#
# Needs a kmem-bindable dax device on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_provision
pn_require_dax
ktap_print_msg "using $DAX on private node $PN (state was: $(pn_state))"
pn_reset
if [ "$(pn_state)" != unplugged ]; then
	ktap_skip_all "$DAX: cannot reach 'unplugged' baseline (memory in use?)"
	exit "$KSFT_SKIP"
fi

ktap_set_plan 3

# 1. plugging in brings the node up, and it comes up private
pn_hotplug online_kernel; rc=$?
if [ "$rc" = 0 ] && [ "$(pn_state)" = online_kernel ] && pn_online_private; then
	ktap_test_pass "plugged in; node $PN is online and private"
else
	ktap_test_fail "failed to plug in: rc=$rc state=$(pn_state) online=$(pn_node_online && echo 1 || echo 0)"
fi

# 2. unplug takes the memory away again
pn_hotplug unplugged; rc=$?
if [ "$rc" = 0 ] && [ "$(pn_state)" = unplugged ] && ! pn_node_online; then
	ktap_test_pass "unplug offlines private node $PN"
else
	ktap_test_fail "unplug did not clear private state: rc=$rc state=$(pn_state) online=$(pn_node_online && echo 1 || echo 0)"
fi

# 3. an invalid hotplug state string is rejected
before=$(pn_state)
pn_hotplug bogus_state; rc=$?
if [ "$rc" != 0 ] && [ "$(pn_state)" = "$before" ]; then
	ktap_test_pass "invalid hotplug state string rejected"
else
	ktap_test_fail "invalid state not rejected: rc=$rc state=$(pn_state)"
fi

pn_reset
ktap_finished
