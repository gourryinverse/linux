#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node capability + hotplug ABI test.
#
#   1. an opt-in is writable while unplugged and reads back
#   2. a bogus bool value is rejected
#   3. demotion without reclaim is accepted by the setter ...
#   4. ... but the inconsistent combo fails to plug in (node stays non-private)
#   5. satisfying the dependency plugs in; node becomes private
#   6. opt-ins are read-only (EBUSY) while plugged in
#   7. unplug clears the private state
#   8. an invalid hotplug state string is rejected
#
# Caps are recorded while unplugged; dependencies (demotion needs reclaim)
# are enforced once at hotplug by node_private_register(), not by the setters.
# Needs a kmem-bindable dax device on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_provision
ktap_print_msg "using $DAX on private node $PN (state was: $(pn_state))"
pn_reset
if [ "$(pn_state)" != unplugged ]; then
	ktap_skip_all "$DAX: cannot reach 'unplugged' baseline (memory in use?)"
	exit "$KSFT_SKIP"
fi

ktap_set_plan 8

# 1. an opt-in is writable while unplugged and reads back
pn_set reclaim 1
if [ "$(pn_get reclaim)" = 1 ]; then
	ktap_test_pass "reclaim opt-in recorded while unplugged"
else
	ktap_test_fail "reclaim opt-in not recorded: reclaim=$(pn_get reclaim)"
fi

# 2. a bogus mm_capabilities value is rejected (kstrtou64), mask unchanged
before=$(cat "$D/mm_capabilities" 2>/dev/null)
echo notanumber > "$D/mm_capabilities" 2>/dev/null; rc=$?
if [ "$rc" != 0 ] && [ "$(cat "$D/mm_capabilities" 2>/dev/null)" = "$before" ]; then
	ktap_test_pass "invalid mm_capabilities value rejected (unchanged)"
else
	ktap_test_fail "invalid mm_capabilities not rejected: rc=$rc caps=$(cat "$D/mm_capabilities" 2>/dev/null)"
fi

# 3. dependency enforced at hotplug, not at write: demotion without reclaim is
#    accepted by the setter ...
pn_reset
pn_set demotion 1; rc=$?
if [ "$rc" = 0 ] && [ "$(pn_get demotion)" = 1 ]; then
	ktap_test_pass "demotion without reclaim accepted at write (rc=0)"
else
	ktap_test_fail "demotion write unexpectedly rejected: rc=$rc"
fi

# 4. ... but the inconsistent combination fails to plug in and leaves the node
#    non-private.
pn_hotplug online_kernel; rc=$?
if [ "$rc" != 0 ] && [ "$(pn_state)" = unplugged ] && ! pn_node_online; then
	ktap_test_pass "demotion without reclaim rejected at hotplug (node stays private=0)"
else
	ktap_test_fail "inconsistent caps plugged in: rc=$rc state=$(pn_state) online=$(pn_node_online && echo 1 || echo 0)"
fi

# 5. satisfying the dependency lets it plug in and the node becomes private
pn_set reclaim 1
pn_hotplug online_kernel; rc=$?
if [ "$rc" = 0 ] && [ "$(pn_state)" = online_kernel ] && pn_online_private; then
	ktap_test_pass "reclaim+demotion plugs in; node $PN is now private"
else
	ktap_test_fail "consistent caps failed to plug in: rc=$rc state=$(pn_state) online=$(pn_node_online && echo 1 || echo 0)"
fi

# 6. opt-ins are read-only (EBUSY) while plugged in
pn_set user_numa 1; rc=$?
if [ "$rc" != 0 ] && [ "$(pn_get user_numa)" = 0 ]; then
	ktap_test_pass "opt-in write rejected while plugged in (EBUSY)"
else
	ktap_test_fail "opt-in mutated while online: rc=$rc mbind=$(pn_get user_numa)"
fi

# 7. unplug clears the private state
pn_hotplug unplugged; rc=$?
if [ "$rc" = 0 ] && [ "$(pn_state)" = unplugged ] && ! pn_node_online; then
	ktap_test_pass "unplug offlines private node $PN"
else
	ktap_test_fail "unplug did not clear private state: rc=$rc state=$(pn_state) online=$(pn_node_online && echo 1 || echo 0)"
fi

# 8. an invalid hotplug state string is rejected
before=$(pn_state)
pn_hotplug bogus_state; rc=$?
if [ "$rc" != 0 ] && [ "$(pn_state)" = "$before" ]; then
	ktap_test_pass "invalid hotplug state string rejected"
else
	ktap_test_fail "invalid state not rejected: rc=$rc state=$(pn_state)"
fi

pn_reset
ktap_finished
