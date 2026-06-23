#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# dax/kmem "adistance" knob: retune a private node's abstract distance
# (memory-tier placement) while the device is unplugged.  The attribute is
# exposed only in private mode, alongside the per-service opt-ins.
#
#   1. adistance is visible only while private=1
#   2. read/write adistance while unplugged
#   3. invalid values (<= 0, non-numeric) are rejected
#   4. toggling "private" resets adistance to the probe-time default
#   5. adistance is read-only (-EBUSY) while the device holds memory

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_provision			# leaves the device in private mode
echo unplugged > "$D/state" 2>/dev/null

[ -e "$D/adistance" ] || { ktap_skip_all "no adistance attr (private mode)"; exit "$KSFT_SKIP"; }
ktap_set_plan 5

DEF=$(cat "$D/adistance")
CUSTOM=$((DEF + 100))		# a distinct, valid (>0) distance

# 1. visible only while private=1 ----------------------------------------------
echo 0 > "$D/private" 2>/dev/null
gone=$([ -e "$D/adistance" ] && echo no || echo yes)
echo 1 > "$D/private" 2>/dev/null
back=$([ -e "$D/adistance" ] && echo yes || echo no)
if [ "$gone" = yes ] && [ "$back" = yes ]; then
	ktap_test_pass "adistance present only in private mode (private=0 hides it)"
else
	ktap_test_fail "adistance visibility wrong (hidden@priv0=$gone shown@priv1=$back)"
fi

# 2. read/write while unplugged ------------------------------------------------
echo "$CUSTOM" > "$D/adistance" 2>/dev/null
got=$(cat "$D/adistance")
if [ "$got" = "$CUSTOM" ]; then
	ktap_test_pass "adistance write/read while unplugged (def=$DEF set=$CUSTOM)"
else
	ktap_test_fail "adistance readback $got != $CUSTOM (def=$DEF)"
fi

# 3. invalid values rejected ---------------------------------------------------
bad=0
echo 0    > "$D/adistance" 2>/dev/null && bad=1
echo -5   > "$D/adistance" 2>/dev/null && bad=1
echo junk > "$D/adistance" 2>/dev/null && bad=1
now=$(cat "$D/adistance")
if [ "$bad" = 0 ] && [ "$now" = "$CUSTOM" ]; then
	ktap_test_pass "adistance rejects <=0 / non-numeric and is unchanged ($now)"
else
	ktap_test_fail "adistance accepted an invalid value (bad=$bad now=$now)"
fi

# 4. toggling private resets adistance to the default --------------------------
echo "$CUSTOM" > "$D/adistance" 2>/dev/null
echo 0 > "$D/private" 2>/dev/null		# leave private: reset to default
echo 1 > "$D/private" 2>/dev/null		# re-enter private: attr reappears
r=$(cat "$D/adistance")
if [ "$r" = "$DEF" ]; then
	ktap_test_pass "toggling private resets adistance to default ($DEF)"
else
	ktap_test_fail "toggling private left adistance=$r (want $DEF)"
fi

# 5. -EBUSY while the device is online -----------------------------------------
echo "$CUSTOM" > "$D/adistance" 2>/dev/null
if echo online_movable > "$D/state" 2>/dev/null; then
	if echo "$((CUSTOM + 1))" > "$D/adistance" 2>/dev/null; then
		ktap_test_fail "adistance writable while online (expected -EBUSY)"
	else
		ktap_test_pass "adistance write refused while online (-EBUSY)"
	fi
	echo unplugged > "$D/state" 2>/dev/null
else
	ktap_test_skip "could not online device to test -EBUSY"
fi

ktap_finished
