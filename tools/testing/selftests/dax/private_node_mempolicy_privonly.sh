#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Process-wide set_mempolicy(MPOL_BIND, {private-only nodemask}) must not livelock.
#
# The worry: a MOVABLE allocation confined by the policy to {PN} (a single private
# node) has nowhere to go, so the fault refaults forever.  apply_policy_zone()
# does NOT prevent this -- it only relaxes the ZONE (it drops the nodemask for
# sub-movable/unmovable allocations so page tables etc. can spill; it leaves the
# nodemask in place for a movable allocation).
#
# The reason it does NOT livelock is a SEPARATE mechanism: mpol_set_nodemask()
# marks any bind that leaves the FALLBACK set as MPOL_F_PRIVATE, and
# mpol_alloc_flags() then routes the allocation through ALLOC_ZONELIST_PRIVATE.
# The PRIVATE zonelist CONTAINS PN (it spans every N_MEMORY node, public and
# private), whereas the default FALLBACK zonelist excludes it.  So the movable
# fault confined to {PN} is
# reachable and lands ON PN -- it is not the "free-but-unreachable" wedge that a
# private-only *cpuset.mems* would be (cpuset carries no zonelist, hence the
# >=1-fallback rule there; a mempolicy opts into the private zonelist, so it does
# not need one).
#
#   L1 movable anon fault under process-wide MPOL_BIND({PN}) on a MOVABLE-ONLY
#      private node COMPLETES and lands on PN (reachable via the private zonelist),
#      within a watchdog -- a regression FAILs (bounded) instead of hanging.
#   L2 oversized fault fills PN then OOM-kills the bound task (forward progress),
#      it does not livelock.
#
# Needs one USER_NUMA private node. See private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision
pn_reset

# movable-only private node with userland placement (the worst case for the worry)
pn_set user_numa 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN as movable-only USER_NUMA private"
	pn_reset; exit "$KSFT_SKIP"
fi
ktap_print_msg "private node $PN online_movable user_numa=1 (movable-only)"
ktap_set_plan 3

# per-node PageTables: (kB) -- how much unmovable kernel memory landed on PN.
pt_kb() { awk '$0 ~ /^Node [0-9]+ PageTables:/{print $4}' "$NODE_BASE/node$PN/meminfo" 2>/dev/null; }

# watchdog-run BINDFAULT: run in bg, wait <secs> for "done"; if it never prints,
# it livelocked -> kill and report not-done (FAIL, not hang).
BF=/tmp/pn_privonly.$$
run_bindfault() {	# run_bindfault MB SECS -> sets BF_DONE / BF_ONPN / BF_ALIVE
	: > "$BF"
	"$TOOL" bindfault "$PN" "$1" >"$BF" 2>&1 &
	local j=$! i
	for i in $(seq 1 "$(( $2 * 5 ))"); do
		grep -qE "done|rc=-1|mmap:" "$BF" && break
		kill -0 "$j" 2>/dev/null || break		# died (e.g. OOM)
		sleep 0.2
	done
	BF_ALIVE=$(kill -0 "$j" 2>/dev/null && echo 1 || echo 0)
	kill -9 "$j" 2>/dev/null; wait "$j" 2>/dev/null
	BF_DONE=$(grep -q "done" "$BF" && echo 1 || echo 0)
	BF_ONPN=$(sed -n "s/.*on_node$PN=\([0-9]*\).*/\1/p" "$BF" | head -1)
}

# --- L1: a bounded movable bind completes and lands on PN -----------------
run_bindfault 64 30
sed 's/^/# /' "$BF"
ktap_print_msg "L1 done=$BF_DONE onPN=${BF_ONPN:-0} alive_at_timeout=$BF_ALIVE"
if [ "$BF_DONE" = 1 ] && [ "${BF_ONPN:-0}" -gt 0 ] 2>/dev/null; then
	ktap_test_pass "L1 MPOL_BIND({$PN}) movable fault completed on private $PN (private zonelist reachable; no livelock)"
else
	ktap_test_fail "L1 MPOL_BIND({$PN}) movable fault did not complete on $PN (done=$BF_DONE onPN=$BF_ONPN alive=$BF_ALIVE -- livelock?)"
fi

# --- L2: oversized fault makes forward progress (OOM), not a livelock ------
# Fault more than the private node can hold; with no RECLAIM cap the bound task
# must OOM (CONSTRAINT_MEMORY_POLICY) rather than refault forever.
PNKB=$(sed -n 's/^Node '"$PN"' MemTotal:[[:space:]]*\([0-9]*\) kB/\1/p' "$NODE_BASE/node$PN/meminfo")
OVER=$(( ${PNKB:-1048576} / 1024 + 512 ))		# node size + 512 MB, in MB
run_bindfault "$OVER" 40
sed 's/^/# /' "$BF"
ktap_print_msg "L2 fault=${OVER}MB (node=${PNKB:-?}kB) done=$BF_DONE alive_at_timeout=$BF_ALIVE"
# forward progress = either it completed, or the task was OOM-killed; a livelock is
# "still alive at the watchdog with no done".
if [ "$BF_ALIVE" = 0 ] || [ "$BF_DONE" = 1 ]; then
	ktap_test_pass "L2 oversized MPOL_BIND({$PN}) made forward progress (done=$BF_DONE, OOM if not) -- no livelock"
else
	ktap_test_fail "L2 oversized MPOL_BIND({$PN}) still running at watchdog with no completion -- livelock"
fi

# --- L3: permissive cpuset {public, PN}: kernel/unmovable allocations cannot
#     land on the movable-only private node, so they escape to the public
#     fallback -- no wedge -- while the bound anon still lands on PN. ----------
# This is the exact worry: a process-wide MPOL_BIND({PN}) also binds the task's
# NON-mempolicy kernel allocations (page tables); on a movable-only node those
# unmovable allocations have no zone.  apply_policy_zone() drops the nodemask for
# them (ZONE_NORMAL < the policy's collapsed ZONE_MOVABLE), so they fall back to
# the public node in cpuset.mems instead of refaulting forever.
CG=$(pn_cgroup2) || CG=
D0=$(pn_dram_list | awk '{print $1}')
if [ -z "$CG" ] || [ -z "$D0" ]; then
	ktap_test_skip "L3 needs cgroup2 + a public node"
else
	g="$CG/privonly_l3"; mkdir -p "$g" 2>/dev/null
	cat "$CG/cpuset.cpus.effective" > "$g/cpuset.cpus" 2>/dev/null
	echo "$D0,$PN" > "$g/cpuset.mems" 2>/dev/null		# permissive: fallback + private
	pt0=$(pt_kb); : > "$BF"
	( echo $BASHPID > "$g/cgroup.procs"; exec "$TOOL" bindfault "$PN" 64 15 ) >"$BF" 2>&1 &
	j=$!
	for i in $(seq 1 150); do grep -qE "done|rc=-1|mmap:" "$BF" && break; kill -0 "$j" 2>/dev/null || break; sleep 0.2; done
	pt1=$(pt_kb)
	done3=$(grep -q done "$BF" && echo 1 || echo 0)
	onpn3=$(sed -n "s/.*on_node$PN=\([0-9]*\).*/\1/p" "$BF" | head -1)
	echo $$ > "$CG/cgroup.procs" 2>/dev/null; kill -9 "$j" 2>/dev/null; wait "$j" 2>/dev/null
	rmdir "$g" 2>/dev/null
	dpt=$(( ${pt1:-0} - ${pt0:-0} ))
	sed 's/^/# /' "$BF"
	ktap_print_msg "L3 cpuset={$D0,$PN}: done=$done3 onPN=${onpn3:-0} PageTables_delta=${dpt}kB"
	if [ "$done3" = 1 ] && [ "${onpn3:-0}" -gt 0 ] 2>/dev/null && [ "$dpt" -le 128 ] 2>/dev/null; then
		ktap_test_pass "L3 permissive cpuset {$D0,$PN}: anon on $PN, page tables escaped to public (+${dpt}kB) -- no wedge"
	else
		ktap_test_fail "L3 permissive cpuset: unmovable kernel allocs wedged on movable-only $PN (done=$done3 onPN=$onpn3 pt=+${dpt}kB)"
	fi
fi

rm -f "$BF"
pn_reset
ktap_finished
