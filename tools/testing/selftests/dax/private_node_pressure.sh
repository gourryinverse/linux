#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# A private node never receives a NORMAL alloc, and exhausting it via its own
# private bind drives a node-aware OOM kill rather than a silent spill to DRAM.
#
#   1. Containment: under node-0 anon pressure (> node-0 DRAM) plus a per-node
#      hugetlb write (a __GFP_THISNODE alloc), the private node stays free==total.
#   2. Directed OOM: reclaim opted-in, no swap, exhaust via the {P} bind --
#      must OOM-kill a {P}-eligible victim, not spill onto another node.
#   3. THP mbind: a __GFP_THISNODE THP fault under pressure stays on-node.
#
# Needs a kmem-bindable dax device on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

# pages of zoneinfo key $2 ("free"/"managed") summed over node $1
zone_pages() { awk -v n="$1" -v k="$2" '$1=="Node"{i=($2==n",")} i&&$1==k{m+=$2} END{print m+0}' /proc/zoneinfo; }
node_total_kb() { awk '/MemTotal:/{print $4}' "$NODE_BASE/node$1/meminfo"; }
node_free_kb()  { awk '/MemFree:/{print $4}'  "$NODE_BASE/node$1/meminfo"; }

pn_begin
pn_require_tool
pn_provision
pn_reset

# Restore any global toggle / swap state changed below, on every exit path.
pn_snapshot_swaps
trap pn_restore_globals EXIT INT TERM

ktap_set_plan 3

# ---------------------------------------------------------------------------
# 1. CONTAINMENT: no-caps node, online_kernel (most permissive zone).
# ---------------------------------------------------------------------------
pn_hotplug online_kernel
if [ "$(pn_state)" != online_kernel ] || ! pn_is_private; then
	ktap_test_skip "could not online node $PN as private (state=$(pn_state))"
else
	tot=$(node_total_kb "$PN")
	free0=$(node_free_kb "$PN")
	spill=0
	[ "${tot:-0}" -gt 0 ] || spill=1
	[ "$free0" = "$tot" ] || spill=1

	# per-node hugetlb pool write: an explicit __GFP_THISNODE alloc must place 0.
	hp="$NODE_BASE/node$PN/hugepages/hugepages-2048kB/nr_hugepages"
	if [ -w "$hp" ]; then
		hp0=$(cat "$hp" 2>/dev/null); pn_save_global "$hp"
		echo 64 > "$hp" 2>/dev/null
		[ "$(cat "$hp")" = 0 ] || spill=1
		[ "$(node_free_kb "$PN")" = "$tot" ] || spill=1
		echo "${hp0:-0}" > "$hp" 2>/dev/null
	fi

	# node-0 anon pressure > node-0 DRAM, walked gradually so reclaim keeps up
	# instead of OOMing the churn.  Anon reclaims only via swap, so best-effort
	# provide some (the containment verdict holds either way).
	pn_swap_setup
	churn_mb=$(( $(node_total_kb 0) / 1024 + 512 ))
	"$TOOL" churn "$churn_mb" 18 >/dev/null 2>&1 &
	ch=$!
	for _ in 1 2 3 4 5 6; do
		sleep 3
		[ "$(node_free_kb "$PN")" = "$tot" ] || spill=1
	done
	kill "$ch" 2>/dev/null; wait "$ch" 2>/dev/null

	if [ "$spill" = 0 ]; then
		ktap_test_pass "private node $PN untouched under node-0 pressure + THISNODE hugetlb"
	else
		ktap_test_fail "containment breach: node $PN free dropped below total"
	fi
fi
pn_reset

# ---------------------------------------------------------------------------
# 2. DIRECTED OOM: reclaim-opted node, no swap, exhaust via the private bind.
# ---------------------------------------------------------------------------
swapoff -a 2>/dev/null			# the contract under test is no-reclaim-target
pn_set reclaim 1
pn_set hotunplug 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "could not online node $PN (reclaim) as private"
else
	man_mb=$(( $(zone_pages "$PN" managed) / 256 ))
	if [ "${man_mb:-0}" -lt 64 ]; then
		ktap_test_skip "node $PN too small (${man_mb}MB) for the OOM experiment"
	else
		hog_mb=$(( man_mb * 75 / 100 ))
		trig_mb=$(( man_mb * 50 / 100 ))
		"$TOOL" daxmap "/dev/$DAX" "$hog_mb" "$PN" 120 >/dev/null 2>&1 &
		hp=$!
		sleep 4
		"$TOOL" daxmap "/dev/$DAX" "$trig_mb" "$PN" 0 >/dev/null 2>&1 &
		tp=$!
		wait "$tp"; trc=$?
		kill -0 "$hp" 2>/dev/null && hog_alive=1 || hog_alive=0
		kill -9 "$hp" 2>/dev/null; wait "$hp" 2>/dev/null

		# Contract: a {P}-bound exhaustion must drive a node-scoped OOM, never
		# a silent spill.  WHICH faulter the OOM killer picks is a race (oom_badness
		# ranks by whole-process RSS); both kills are valid.  The only failure is
		# the trigger COMPLETING with no OOM kill at all.
		if [ "$trc" = 0 ] && [ "$hog_alive" = 0 ]; then
			ktap_test_pass "node-scoped OOM killed the hog; trigger completed on node $PN"
		elif [ "$trc" = 137 ]; then
			ktap_test_pass "node-scoped OOM killed the {$PN}-bound trigger (no DRAM spill)"
		elif [ "$trc" = 0 ]; then
			ktap_test_fail "trigger completed with no OOM kill - likely spilled off node $PN"
		else
			ktap_test_fail "unexpected OOM outcome (trigger rc=$trc hog_alive=$hog_alive)"
		fi
	fi
fi
pn_reset

# ---------------------------------------------------------------------------
# 3. THP-order mbind under private-node pressure must stay on-node.  A private
#    node has no NOFALLBACK list, so the __GFP_THISNODE THP fault is served from
#    ZONELIST_PRIVATE and must stay confined to the node.
# ---------------------------------------------------------------------------
pn_set user_numa 1
pn_set hotunplug 1
pn_hotplug online_movable
pn_save_global /sys/kernel/mm/transparent_hugepage/enabled
echo always > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "could not online node $PN (mbind) as private"
elif ! grep -q '\[always\]' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null; then
	ktap_test_skip "THP unavailable (CONFIG_TRANSPARENT_HUGEPAGE)"
else
	man_mb=$(( $(zone_pages "$PN" managed) / 256 ))
	# Pressure the node so the THP fast-attempt can't get a local 2MB block.
	"$TOOL" mbind "$PN" $(( man_mb * 90 / 100 )) 30 >/dev/null 2>&1 &
	fp=$!; sleep 3
	out=$("$TOOL" mbindthp "$PN" 64 0 2>&1); rc=$?
	kill "$fp" 2>/dev/null; wait "$fp" 2>/dev/null
	echo "$out" | sed 's/^/# /'
	off=$(echo "$out" | pn_field off)
	if [ "$rc" != 0 ]; then
		ktap_test_pass "THP mbind failed rather than spilling off private node $PN (rc=$rc)"
	elif [ "${off:-1}" = 0 ]; then
		ktap_test_pass "THP-order mbind stayed on private node $PN under pressure (off=0)"
	else
		ktap_test_fail "THP mbind spilled $off pages off private node $PN"
	fi
fi
pn_reset

ktap_finished
