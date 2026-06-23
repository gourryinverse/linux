#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test that private (N_MEMORY_PRIVATE) nodes are not partitioned by
# cpuset.mems and a cpuset-forbidden non-private nodes stays unreachable.
#
#      {P} = Private bind     {D} = DRAM bind
#  T1) a private node cannot become effective in cpuset.mems.
#  T2) mbind({P}) works from a cpuset that excludes P (private not gated).
#  T3) move_pages onto P works from a restricted cpuset.
#  T4) mbind({D}) to a cpuset-excluded PUBLIC node is still EINVAL.
#  T5) a private bind is immune to public cpuset.mems changes.
#  T6) a general (no-policy) allocation never lands on the private node.
#
# Needs a private node and two public nodes + cgroup2 cpuset.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision
pn_reset
CG=$(pn_cgroup2) || { ktap_skip_all "cgroup2 cpuset unavailable"; pn_reset; exit "$KSFT_SKIP"; }

pn_set user_numa 1
pn_set hotunplug 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN as private"
	pn_reset; exit "$KSFT_SKIP"
fi
CPUS=$(cat "$CG/cpuset.cpus.effective")
DRAM=$(pn_dram_list); set -- $DRAM; D0=$1; D1=${2:-}
ktap_print_msg "private node $PN; DRAM={$DRAM} D0=$D0 D1=${D1:-none}"
ktap_set_plan 6

HF=/tmp/pn_do.$$
mkchild() { mkdir -p "$1" 2>/dev/null; echo "$CPUS" > "$1/cpuset.cpus" 2>/dev/null; echo "$D0" > "$1/cpuset.mems" 2>/dev/null; }

# --- T1: private node rejected from cpuset.mems ------------------------------
g="$CG/do1"; mkchild "$g"
echo "$D0,$PN" > "$g/cpuset.mems" 2>/dev/null; rc=$?
eff=$(cat "$g/cpuset.mems.effective" 2>/dev/null)
ktap_print_msg "T1 write {$D0,$PN} rc=$rc eff='$eff'"
# private nodes are outside cpuset's universe: a configured value may store but
# is dropped from effective_mems, so $PN can never become effective.
if ! nodelist_has "$eff" "$PN"; then
	ktap_test_pass "T1 private node $PN cannot be effective in cpuset.mems (eff='$eff')"
else
	ktap_test_fail "T1 cpuset.mems made private node $PN effective (rc=$rc eff='$eff')"
fi
rmdir "$g" 2>/dev/null

# --- T2: mbind to opted private works from a cpuset that excludes it ----------
g="$CG/do2"; mkchild "$g"		# mems={D0}, no P
out=$( ( echo $BASHPID > "$g/cgroup.procs"; exec "$TOOL" mbind "$PN" 16 ) 2>&1 )
echo "$out" | sed 's/^/# /'; echo $$ > "$CG/cgroup.procs" 2>/dev/null; rmdir "$g" 2>/dev/null
placed=$(echo "$out" | pn_field "on_node$PN")
if echo "$out" | grep -q "rc=0" && [ "${placed:-0}" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "T2 mbind({$PN}) placed $placed pages from a cpuset excluding $PN (cpuset does not gate private)"
else
	ktap_test_fail "T2 mbind({$PN}) failed from restricted cpuset ($out)"
fi

# --- T3: move_pages onto opted private works from a restricted cpuset ---------
g="$CG/do3"; mkchild "$g"
out=$( ( echo $BASHPID > "$g/cgroup.procs"; exec "$TOOL" movepagesto "$PN" ) 2>&1 )
echo "$out" | sed 's/^/# /'; echo $$ > "$CG/cgroup.procs" 2>/dev/null; rmdir "$g" 2>/dev/null
if echo "$out" | grep -q "status=$PN" || echo "$out" | grep -q "now on node$PN"; then
	ktap_test_pass "T3 move_pages onto $PN works from a cpuset excluding it"
else
	ktap_test_fail "T3 move_pages onto $PN failed from restricted cpuset ($out)"
fi

# --- T4: public node outside the cpuset is still gated (backstop) -------------
if [ -z "$D1" ]; then
	ktap_test_skip "T4 needs a second public node"
else
	g="$CG/do5"; mkchild "$g"	# mems={D0}; D1 excluded
	out=$( ( echo $BASHPID > "$g/cgroup.procs"; exec "$TOOL" mbind "$D1" 8 ) 2>&1 )
	echo "$out" | sed 's/^/# /'; echo $$ > "$CG/cgroup.procs" 2>/dev/null; rmdir "$g" 2>/dev/null
	if echo "$out" | grep -q "errno=22"; then
		ktap_test_pass "T4 mbind({$D1}) on a cpuset-excluded PUBLIC node still EINVAL (public stays gated)"
	else
		ktap_test_fail "T4 public node $D1 reachable despite cpuset exclusion ($out)"
	fi
fi

# --- T5: private bind immune to public cpuset.mems changes -------------------
if [ -z "$D1" ]; then
	ktap_test_skip "T5 needs a second public node"
else
	g="$CG/do6"; mkchild "$g"	# mems={D0}
	: > "$HF"
	( echo $BASHPID > "$g/cgroup.procs"; exec "$TOOL" mbindhold bind "$PN" 16 20 ) >"$HF" 2>&1 &
	J=$!
	for _ in $(seq 1 30); do grep -q "pid=" "$HF" && break; sleep 0.2; done
	HPID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$HF" | head -1)
	HADDR=$(sed -n 's/.*addr=\(0x[0-9a-f]*\).*/\1/p' "$HF" | head -1)
	onP0=$(nm_on_node "$HPID" "$HADDR" "$PN")
	echo "$D0,$D1" > "$g/cpuset.mems" 2>/dev/null	# change PUBLIC mems
	sleep 2
	onP1=$(nm_on_node "$HPID" "$HADDR" "$PN"); pol=$(nm_policy "$HPID" "$HADDR")
	sed 's/^/# /' "$HF"
	ktap_print_msg "T5 onP:$onP0->$onP1 pol=$pol"
	echo $$ > "$CG/cgroup.procs" 2>/dev/null; kill -9 "$J" 2>/dev/null; wait "$J" 2>/dev/null; rmdir "$g" 2>/dev/null
	if nodelist_has "${pol#*:}" "$PN" && [ "${onP1:-0}" -gt 0 ] 2>/dev/null; then
		ktap_test_pass "T5 private bind unaffected by public cpuset.mems change (pol=$pol onP:$onP0->$onP1)"
	else
		ktap_test_fail "T5 private bind disturbed by public mems change (pol=$pol onP:$onP0->$onP1)"
	fi
fi

# --- T6: general allocation still excluded from the private node --------------
out=$("$TOOL" anon 8 0 2>&1); echo "$out" | sed 's/^/# /'
land=$(echo "$out" | sed -n 's/.*first page node=\([0-9-]*\).*/\1/p')
if echo "$out" | grep -q "populated" && [ "${land:-$PN}" != "$PN" ]; then
	ktap_test_pass "T6 general allocation landed on public node $land, not private $PN"
else
	ktap_test_fail "T6 general allocation landed on private $PN ($out)"
fi

rm -f "$HF"
pn_reset
ktap_finished
