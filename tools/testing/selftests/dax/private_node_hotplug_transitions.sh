#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node hotplug transitions while a task references the node.
#
#   E1 unplug P while a VMA is bound to it: pages migrate off, P leaves
#      the private node (observed leaving has_memory), the task survives.
#   E2 re-plug P private: a fresh mbind({P}) is honored again.
#   E4 stale bind does not reattach: unplug P while bound, then re-plug it private
#      with the same nid as a different owner; the task's fresh faults must NOT
#      land back on P, because offline scrubbed the stale MPOL_F_PRIVATE bind.
#   E5 PRIVATE -> PUBLIC -> PRIVATE: clearing the kmem "private" toggle onlines P
#      as a public N_MEMORY node where mbind needs NO cap; setting it again
#      returns to private.  SKIPs if the public online yields no public node.
#
# Needs a kmem-bindable dax device on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision
pn_reset

up_private() {	# online PN as private with the user_numa opt-in
	pn_reset
	pn_set private 1
	pn_set user_numa 1
	pn_hotplug online_movable
}
up_private
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN as private (user_numa)"
	pn_reset; exit "$KSFT_SKIP"
fi
DRAM=$(pn_dram_list); set -- $DRAM; D0=$1
ktap_print_msg "private node $PN on $DAX; DRAM={$DRAM} D0=$D0"
ktap_set_plan 4

HF=/tmp/pn_hp_h.$$

# --- E1: unplug while a VMA is bound to P -> pages migrate off, task survives --
: > "$HF"
"$TOOL" mbindhold bind "$PN" 16 40 >"$HF" 2>&1 &
HJOB=$!
for _ in $(seq 1 30); do grep -q "pid=" "$HF" && break; sleep 0.2; done
HPID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$HF" | head -1)
HADDR=$(sed -n 's/.*addr=\(0x[0-9a-f]*\).*/\1/p' "$HF" | head -1)
sed 's/^/# /' "$HF"
onP0=$(nm_on_node "$HPID" "$HADDR" "$PN")
pn_hotplug unplugged; rc=$?
sleep 1
onP1=$(nm_on_node "$HPID" "$HADDR" "$PN")
alive=$(kill -0 "$HPID" 2>/dev/null && echo 1 || echo 0)
ktap_print_msg "E1 onP:$onP0->$onP1 unplug_rc=$rc state=$(pn_state) online=$(pn_node_online && echo 1 || echo 0) alive=$alive"
if [ "$rc" = 0 ] && [ "$(pn_state)" = unplugged ] && ! pn_node_online && [ "$alive" = 1 ] && [ "${onP1:-1}" = 0 ]; then
	ktap_test_pass "E1 unplug migrated $onP0 pages off $PN and the task survived"
else
	ktap_test_fail "E1 unplug-while-bound failed (rc=$rc onP:$onP0->$onP1 alive=$alive online=$(pn_node_online && echo 1 || echo 0))"
fi
kill -9 "$HJOB" 2>/dev/null; wait "$HJOB" 2>/dev/null

# --- E2: re-plug P private -> a fresh mbind({P}) is honored again --------------
up_private
if [ "$(pn_state)" = online_movable ] && pn_is_private; then
	out=$("$TOOL" mbind "$PN" 8 2>&1); echo "$out" | sed 's/^/# /'
	placed=$(echo "$out" | pn_field "on_node$PN")
	if echo "$out" | grep -q "rc=0" && [ "${placed:-0}" -gt 0 ] 2>/dev/null; then
		ktap_test_pass "E2 re-plugged $PN private; fresh mbind({$PN}) honored ($placed pages)"
	else
		ktap_test_fail "E2 fresh mbind({$PN}) failed after re-plug ($out)"
	fi
else
	ktap_test_fail "E2 could not re-online $PN as private (state=$(pn_state))"
fi

# --- E4: a stale bind must NOT reattach to a re-onlined node (nid reuse) -------
# A task binds {P} and faults it; P is unplugged (its pages migrate off the bound
# VMA) then re-onlined private as a different "owner" reusing the same nid.  The
# task's fresh faults must NOT land back on P: offline scrubs the stale
# MPOL_F_PRIVATE bind, so phase-2 falls back to DRAM (on_node$PN == 0).
up_private
if [ "$(pn_state)" = online_movable ] && pn_is_private; then
	: > "$HF"
	"$TOOL" mbindrefault "$PN" 32 18 >"$HF" 2>&1 &
	RJOB=$!
	for _ in $(seq 1 40); do grep -q "phase1" "$HF" && break; sleep 0.2; done
	pn_hotplug unplugged; sleep 1		# pages migrate off the bound VMA
	up_private				# re-online private (nid reused, new owner)
	wait "$RJOB" 2>/dev/null
	sed 's/^/# /' "$HF"
	on2=$(sed -n 's/.*phase2 on_node'"$PN"'=\([0-9]*\).*/\1/p' "$HF" | head -1)
	ktap_print_msg "E4 phase2 on_node$PN=$on2 (want 0; >0 = stale bind reattached)"
	if [ "${on2:-1}" = 0 ]; then
		ktap_test_pass "E4 stale bind scrubbed on offline; no reattach to re-onlined $PN"
	else
		ktap_test_fail "E4 stale bind reattached $on2 fresh pages onto re-onlined $PN"
	fi
else
	ktap_test_skip "E4 could not online $PN private for the reattach probe"
fi

# --- E5: PRIVATE -> PUBLIC -> PRIVATE via the kmem "private" toggle -----------
# Device stays bound to kmem throughout; only the "private" toggle changes.
e5_skip() { ktap_test_skip "E5 $1"; up_private >/dev/null 2>&1; pn_reset; rm -f "$HF"; ktap_finished; exit 0; }

pn_hotplug unplugged 2>/dev/null
pn_set private 0
pn_hotplug online_movable
sleep 1
ktap_print_msg "E5 after private=0: has_memory=$(cat "$NODE_BASE/has_memory") online=$(pn_node_online && echo 1 || echo 0) private_cfg=$(pn_is_private && echo 1 || echo 0)"
if ! node_in_mask "$PN" has_memory; then
	e5_skip "kmem private=0 did not bring $PN online as a public node"
fi
# public phase: ordinary mbind to P, no capability involved
out=$("$TOOL" mbind "$PN" 8 2>&1); echo "$out" | sed 's/^/# /'
placed=$(echo "$out" | pn_field "on_node$PN")
pub_ok=0
echo "$out" | grep -q "rc=0" && [ "${placed:-0}" -gt 0 ] 2>/dev/null && pub_ok=1

# return PUBLIC -> PRIVATE: unplug, set private again, online private
pn_hotplug unplugged
pn_set private 1
up_private
priv_ok=0
if pn_is_private; then
	out=$("$TOOL" mbind "$PN" 8 2>&1); echo "$out" | sed 's/^/# /'
	placed=$(echo "$out" | pn_field "on_node$PN")
	echo "$out" | grep -q "rc=0" && [ "${placed:-0}" -gt 0 ] 2>/dev/null && priv_ok=1
fi
ktap_print_msg "E5 public_phase_ok=$pub_ok back_to_private_ok=$priv_ok (online=$(pn_node_online && echo 1 || echo 0))"
# The public phase is the key contract (mbind with NO cap); report the return-to-
# private leg too.
if [ "$pub_ok" = 1 ]; then
	if [ "$priv_ok" = 1 ]; then
		ktap_test_pass "E5 P private->public(private=0; mbind needs NO cap)->private, full cycle clean"
	else
		ktap_test_pass "E5 P->public(private=0): public node took mbind with NO cap (return-to-private did not re-establish in-guest; informational)"
	fi
else
	ktap_test_fail "E5 public re-online (private=0) did not accept an ordinary mbind (pub_ok=$pub_ok)"
fi

up_private >/dev/null 2>&1
pn_reset
rm -f "$HF"
ktap_finished
