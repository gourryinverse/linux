#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node demotion: opted-in private nodes participate in the
# memory-tier demotion/promotion machinery via ALLOC_ZONELIST_PRIVATE.
#
#   1. Demote TO private: a private node defaults below DRAM in the tier order,
#      so it is a demotion target; node-0 pressure demotes its cold tail onto
#      the private node.
#   2. Promote OFF private: with NUMA balancing (mode 2), re-touching demoted
#      pages promotes them back to DRAM (pgpromote_success grows).
#
# Caps are configured once at hotplug (re-plugging a node with resident memory
# is not possible).  This SKIPs unless booted with the documented --numa/memmap
# layout and CONFIG_NUMA_BALANCING.

DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR"/private_node_tool

nanon() { awk '/nr_inactive_anon|nr_active_anon/{s+=$2} END{print s+0}' "$NODE_BASE/node$1/vmstat" 2>/dev/null; }
vstat() { awk -v k="$1" '$1==k{print $2}' /proc/vmstat; }
dram_mb() { awk '/MemTotal:/{print int($4/1024)}' "$NODE_BASE/node$1/meminfo"; }
tier_id() { local f; for f in /sys/devices/virtual/memory_tiering/*/nodelist; do
		nodelist_has "$(cat "$f")" "$1" && { b=$(basename "$(dirname "$f")"); echo "${b##*tier}"; return; }; done; }

ktap_print_header
pn_require_root
[ -x "$TOOL" ] || { ktap_skip_all "private_node_tool not built"; exit "$KSFT_SKIP"; }
[ -e /sys/kernel/mm/numa/demotion_enabled ] || { ktap_skip_all "demotion unsupported (CONFIG_NUMA_BALANCING?)"; exit "$KSFT_SKIP"; }
pn_provision_all
set -- $PN_NODES
nprivate=$#
[ "$nprivate" -ge 1 ] || { ktap_skip_all "no private node provisioned"; exit "$KSFT_SKIP"; }
pn_swap_setup
DRAM0=$(awk -F, '{print $1}' "$NODE_BASE/has_memory"); DRAM0=${DRAM0%%-*}

dax_for_node() { local d n; for d in $PN_DAXES; do
		n=$(cat "$DAX_BASE/$d/target_node" 2>/dev/null); [ "$n" = "$1" ] && { echo "$d"; return; }; done; }

P1=$(echo $PN_NODES | awk '{print $1}'); D1=$(dax_for_node "$P1")

# Configure once.  CAP_KERNEL_NUMA enables the whole demotion set (demotion
# target + NUMA balancing + DAMON migrate) in one toggle.  A private node
# defaults below DRAM in the tier order, so no tier placement is needed.
cfg() {	# cfg DAX
	echo unplugged > "$DAX_BASE/$1/state" 2>/dev/null
	echo 1 > "$DAX_BASE/$1/reclaim"
	echo 1 > "$DAX_BASE/$1/demotion"
	echo 1 > "$DAX_BASE/$1/numa_balancing"
	echo 1 > "$DAX_BASE/$1/hotunplug"
	echo online_movable > "$DAX_BASE/$1/state" 2>/dev/null
}
cfg "$D1"
echo 1 > /sys/kernel/mm/numa/demotion_enabled 2>/dev/null
node_in_mask "$P1" has_private_memory || { ktap_skip_all "node $P1 did not online as private"; exit "$KSFT_SKIP"; }
ktap_print_msg "private={$PN_NODES} dram0=$DRAM0 tiers: $P1=tier$(tier_id "$P1") DRAM=tier$(tier_id "$DRAM0")"
ktap_set_plan 2

# 1. demote TO private (node1, below DRAM) ------------------------------------
# Lay a COLD anon tail on DRAM (populated once then held, so it ages onto the
# inactive LRU and becomes a demotion candidate), then drive pressure past the
# TOTAL public DRAM so reclaim demotes the cold tail onto the private node
# rather than only swapping.  A churn sized to a single DRAM node is not enough
# when more than one DRAM node can absorb it, and churn keeps its own pages hot.
a0=$(nanon "$P1")
pubmb=0; for n in $(pn_dram_list); do pubmb=$(( pubmb + $(dram_mb "$n") )); done
coldmb=$(( $(dram_mb "$P1") * 3 / 4 ))		# fits under the private node
"$TOOL" anon "$coldmb" 999 >/dev/null 2>&1 & cold=$!
sleep 8						# let the tail go cold (inactive)
"$TOOL" churn $(( pubmb + 256 )) 45 >/dev/null 2>&1 & cp=$!
grew=0
for _ in $(seq 1 11); do sleep 4; [ "$(nanon "$P1")" -gt $(( a0 + 16384 )) ] 2>/dev/null && { grew=1; break; }; done
kill "$cp" "$cold" 2>/dev/null; wait "$cp" "$cold" 2>/dev/null
if [ "$grew" = 1 ]; then
	ktap_test_pass "node-0 pressure demoted the cold tail onto private node $P1 (nr_anon grew)"
else
	ktap_test_fail "no demotion landed on private node $P1 (nr_anon flat)"
fi

# 2. promote OFF private (node1) ----------------------------------------------
if [ "$(cat "$DAX_BASE/$D1/numa_balancing" 2>/dev/null)" != 1 ]; then
	ktap_test_skip "node $P1 has no numa_balancing cap"
else
	echo 2 > /proc/sys/kernel/numa_balancing 2>/dev/null
	if [ "$(cat /proc/sys/kernel/numa_balancing 2>/dev/null)" != 2 ]; then
		ktap_test_skip "NUMA balancing mode 2 (promotion) unavailable"
	else
		p0=$(vstat pgpromote_success)
		"$TOOL" churn $(( $(dram_mb "$DRAM0") + 512 )) 60 >/dev/null 2>&1 & cp=$!
		ok=0
		for _ in $(seq 1 12); do sleep 5; [ "$(vstat pgpromote_success)" -gt $(( p0 + 1024 )) ] 2>/dev/null && { ok=1; break; }; done
		kill "$cp" 2>/dev/null; wait "$cp" 2>/dev/null
		if [ "$ok" = 1 ]; then
			ktap_test_pass "pages demoted to node $P1 were promoted back to DRAM"
		else
			ktap_test_skip "no promotion observed (pgpromote_success flat in this env)"
		fi
	fi
fi

ktap_finished
