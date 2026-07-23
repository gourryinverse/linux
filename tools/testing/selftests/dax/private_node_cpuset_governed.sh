#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# cpuset governs ALL nodes, private included (the governed model).  A private
# node is a first-class cpuset.mems citizen: cpuset MEMBERSHIP governs whether a
# task may place memory there, the USER_NUMA cap gates whether userland numactl
# ops are allowed at all, and the zonelist keeps GENERAL allocations off private
# nodes regardless of membership.  These are three independent constraints.
#
#      {P} = private bind      {D} = DRAM (public) bind
#  G1) a private node is ACCEPTED into cpuset.mems (as long as the set keeps a
#      fallback node): cpuset governs private nodes, it does not exclude them.
#  G2) a fresh cpuset's effective mems INCLUDES the private node (governable set
#      == N_MEMORY, not just the fallback nodes).
#  G3) mbind({P}) SUCCEEDS from a cpuset that grants P (membership permits it).
#  G4) mbind({P}) is REJECTED (-EINVAL) from a cpuset that excludes P: cpuset
#      membership governs private placement (the deny direction).
#  G5) move_pages(2) onto P succeeds from a cpuset that grants P.
#  G6) migrate_pages(2) onto P succeeds from a cpuset that grants P.
#  G7) a general allocation never lands on P even when P is in cpuset.mems --
#      membership does not defeat zonelist isolation.
#  G8) an opted-out (no USER_NUMA) private node cannot be mbind()'d to even when
#      it is in cpuset.mems (-EINVAL): the USER_NUMA gate is separate from
#      membership.
#  G9) offlining a driver-owned node holding a dax_file bind migrates its pages
#      off and scrubs the bind -- the rebind fires because the private offline
#      shrinks node_states[N_MEMORY] (private nodes are N_MEMORY members), so the
#      top_cpuset effective-mems change drives cpuset_update_tasks_nodemask().
#
# Needs a private node + two public nodes + cgroup2 cpuset.

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
pn_set dax_file 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN as private"
	pn_reset; exit "$KSFT_SKIP"
fi

CPUS=$(cat "$CG/cpuset.cpus.effective")
DRAM=$(pn_dram_list); set -- $DRAM; D0=$1; D1=${2:-}
ktap_print_msg "private node $PN (user_numa=1); DRAM={$DRAM} D0=$D0 D1=${D1:-none}"
ktap_set_plan 9

HF=/tmp/pn_gov.$$
mkchild() {	# mkchild DIR MEMS
	mkdir -p "$1" 2>/dev/null
	echo "$CPUS" > "$1/cpuset.cpus" 2>/dev/null
	echo "$2"    > "$1/cpuset.mems" 2>/dev/null
}
run_in() {	# run_in CGDIR VERB...
	local g=$1; shift
	( echo $BASHPID > "$g/cgroup.procs"; exec "$TOOL" "$@" ) 2>&1
	echo $$ > "$CG/cgroup.procs" 2>/dev/null
}

# --- G1: a private node is ACCEPTED into cpuset.mems (with a fallback) --------
g="$CG/g1"; mkchild "$g" "$D0"
if echo "$D0,$PN" > "$g/cpuset.mems" 2>/dev/null && \
   nodelist_has "$(cat "$g/cpuset.mems.effective")" "$PN"; then
	ktap_test_pass "G1 private $PN accepted into cpuset.mems (eff='$(cat "$g/cpuset.mems.effective")')"
else
	ktap_test_fail "G1 private $PN rejected from cpuset.mems (governed model should accept it)"
fi
rmdir "$g" 2>/dev/null

# --- G2: a fresh cpuset's effective mems INCLUDES the private node ------------
g="$CG/g2"; mkdir -p "$g" 2>/dev/null
gset=$(cat "$g/cpuset.mems.effective" 2>/dev/null); rmdir "$g" 2>/dev/null
ktap_print_msg "G2 fresh effective mems='$gset'"
if nodelist_has "$gset" "$PN"; then
	ktap_test_pass "G2 private $PN present in the default governable set (='$gset')"
else
	ktap_test_fail "G2 private $PN absent from the default governable set (='$gset')"
fi

# --- G3: mbind({P}) succeeds from a cpuset that GRANTS P ----------------------
g="$CG/g3"; mkchild "$g" "$D0,$PN"		# cpuset grants the private node
out=$(run_in "$g" mbind "$PN" 16); rmdir "$g" 2>/dev/null
echo "$out" | sed 's/^/# /'
placed=$(echo "$out" | pn_field "on_node$PN")
if echo "$out" | grep -q "rc=0" && [ "${placed:-0}" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "G3 mbind({$PN}) placed $placed pages when granted by cpuset"
else
	ktap_test_fail "G3 mbind({$PN}) failed despite cpuset granting $PN ($out)"
fi

# --- G4: mbind({P}) is REJECTED from a cpuset that EXCLUDES P -----------------
g="$CG/g4"; mkchild "$g" "$D0"			# cpuset excludes the private node
out=$(run_in "$g" mbind "$PN" 8); rmdir "$g" 2>/dev/null
echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "errno=22"; then
	ktap_test_pass "G4 mbind({$PN}) is -EINVAL when cpuset excludes $PN (membership governs)"
else
	ktap_test_fail "G4 mbind({$PN}) not rejected by a cpuset excluding $PN ($out)"
fi

# --- G5: move_pages(2) onto granted P succeeds -------------------------------
g="$CG/g5"; mkchild "$g" "$D0,$PN"
out=$(run_in "$g" movepagesto "$PN"); rmdir "$g" 2>/dev/null
echo "$out" | sed 's/^/# /'
if echo "$out" | grep -q "(moved)" && echo "$out" | grep -q "now on node$PN"; then
	ktap_test_pass "G5 move_pages onto granted private $PN succeeded"
else
	ktap_test_fail "G5 move_pages onto granted private $PN failed ($out)"
fi

# --- G6: migrate_pages(2) onto granted P succeeds ----------------------------
g="$CG/g6"; mkchild "$g" "$D0,$PN"
out=$(run_in "$g" migratepages2 "$D0" "$PN" 8); rmdir "$g" 2>/dev/null
echo "$out" | sed 's/^/# /'
onnew=$(echo "$out" | pn_field "on_new$PN")
if [ "${onnew:-0}" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "G6 migrate_pages(2) onto granted private $PN moved pages (on_new$PN=$onnew)"
else
	ktap_test_fail "G6 migrate_pages(2) onto granted private $PN placed nothing ($out)"
fi

# --- G7: a general allocation never lands on P even when P is granted ---------
g="$CG/g7"; mkchild "$g" "$D0,$PN"		# P IS in the cpuset...
out=$(run_in "$g" anon 8 0); rmdir "$g" 2>/dev/null
echo "$out" | sed 's/^/# /'
land=$(echo "$out" | sed -n 's/.*first page node=\([0-9-]*\).*/\1/p')
if echo "$out" | grep -q "populated" && [ "${land:-$PN}" != "$PN" ]; then
	ktap_test_pass "G7 general allocation landed on public $land, not granted-but-private $PN (zonelist isolates)"
else
	ktap_test_fail "G7 general allocation landed on private $PN despite zonelist isolation ($out)"
fi

# --- G8: an opted-out (no USER_NUMA) private node in cpuset.mems is unbindable-
pn_hotplug unplugged; pn_set user_numa 0; pn_hotplug online_movable; sleep 1
if [ "$(pn_state)" = online_movable ] && pn_is_private; then
	g="$CG/g8"; mkchild "$g" "$D0,$PN"	# grant P by membership, but no USER_NUMA
	out=$(run_in "$g" mbind "$PN" 8); rmdir "$g" 2>/dev/null
	echo "$out" | sed 's/^/# /'
	if echo "$out" | grep -q "errno=22"; then
		ktap_test_pass "G8 mbind to in-cpuset but non-USER_NUMA $PN is -EINVAL (USER_NUMA gate is separate)"
	else
		ktap_test_fail "G8 non-USER_NUMA private $PN reachable via mbind despite the gate ($out)"
	fi
else
	ktap_test_skip "G8 could not online $PN as opted-out private"
fi

# --- G9: offlining a driver-owned dax_file-bound node scrubs the bind ---------
pn_hotplug unplugged; pn_set user_numa 0; pn_set dax_file 1; pn_hotplug online_movable; sleep 1
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "G9 could not online $PN as opted-out (dax_file) private"
else
	: > "$HF"
	( echo $BASHPID > "$CG/cgroup.procs"; exec "$TOOL" daxmaphold "/dev/$DAX" 16 "$PN" 20 ) >"$HF" 2>&1 &
	J=$!
	for _ in $(seq 1 30); do grep -q "pid=" "$HF" && break; sleep 0.2; done
	HPID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$HF" | head -1)
	HADDR=$(sed -n 's/.*addr=\(0x[0-9a-f]*\).*/\1/p' "$HF" | head -1)
	onP0=$(nm_on_node "$HPID" "$HADDR" "$PN"); pol0=$(nm_policy "$HPID" "$HADDR")
	if [ "${onP0:-0}" -le 0 ] 2>/dev/null || ! nodelist_has "${pol0#*:}" "$PN"; then
		echo $$ > "$CG/cgroup.procs" 2>/dev/null; kill -9 "$J" 2>/dev/null; wait "$J" 2>/dev/null
		ktap_test_skip "G9 dax_file bind not established (on$PN=$onP0 pol='$pol0'); cannot probe scrub"
	else
		pn_hotplug unplugged				# driver-owned $PN leaves N_MEMORY
		for _ in $(seq 1 20); do
			pol1=$(nm_policy "$HPID" "$HADDR")
			[ "$pol1" = "bind:$PN" ] || break
			sleep 0.5
		done
		onP1=$(nm_on_node "$HPID" "$HADDR" "$PN"); pol1=$(nm_policy "$HPID" "$HADDR")
		alive=$(kill -0 "$HPID" 2>/dev/null && echo 1 || echo 0)
		echo $$ > "$CG/cgroup.procs" 2>/dev/null; kill -9 "$J" 2>/dev/null; wait "$J" 2>/dev/null
		sed 's/^/# /' "$HF"
		ktap_print_msg "G9 on:$onP0->$onP1 pol:'$pol0'->'$pol1' alive=$alive"
		case "$pol1" in *:*) nl1="${pol1#*:}";; *) nl1="";; esac
		if [ "${onP1:-0}" -eq 0 ] 2>/dev/null && [ -z "$nl1" ] && [ "$alive" = 1 ]; then
			ktap_test_pass "G9 driver-owned $PN offline migrated pages off and scrubbed the bind to default (pol '$pol0'->'$pol1')"
		else
			ktap_test_fail "G9 driver-owned offline did not scrub-to-default/migrate (on:$onP0->$onP1 pol:'$pol0'->'$pol1' alive=$alive)"
		fi
	fi
fi

rm -f "$HF"
pn_reset
ktap_finished
