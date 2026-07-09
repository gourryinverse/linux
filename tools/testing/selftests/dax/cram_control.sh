#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM debugfs control-surface test: exercises every cram/control command and the
# cram/nodes + counter reads, both for correctness (Part A) and for robustness
# while a demote/promote storm runs (Part B).
#
# cram/control verbs (deep [TEST] introspection):
#   inflate <nid> <n>   reserve n pages on the node (balloon up)
#   deflate <nid> <n>   release n reserved pages (balloon down)
#   block <nid>         stop demotions onto the node (transient migration_blocked)
#   unblock <nid>       resume demotions
#
# Reuses cram_gate_tool (demote an anon region, report whether it landed on cram)
# and cram_pressure_tool (concurrent demote/promote storm with a livelock watchdog).
# Needs a swap device so a blocked demotion has somewhere to spill.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
CTL="$CRAM_DBG/control"
GATE="$DIR/cram_gate_tool"
STORM="$DIR/cram_pressure_tool"

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e "$CTL" ] && [ -e "$CRAM_DBG/nodes" ] ||
	{ ktap_skip_all "cram debugfs control missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$GATE" ] && [ -x "$STORM" ] ||
	{ ktap_skip_all "cram_gate_tool / cram_pressure_tool not built"; exit "$KSFT_SKIP"; }

cram_provision() {
	local d nid drv r
	modprobe -q nd_e820 dax_pmem device_dax nd_pmem 2>/dev/null
	modprobe -q kmem 2>/dev/null
	[ -d /sys/bus/dax/drivers/kmem ] ||
		{ ktap_skip_all "kmem driver unavailable"; exit "$KSFT_SKIP"; }
	if command -v ndctl >/dev/null 2>&1; then
		for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
			ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f \
				>/dev/null 2>&1
		done
	fi
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node"); [ "$nid" -ge 0 ] 2>/dev/null || continue
		node_in_mask "$nid" has_memory && continue
		drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
		[ "$drv" = device_dax ] &&
			basename "$d" > /sys/bus/dax/drivers/device_dax/unbind 2>/dev/null
		[ "$drv" = kmem ] ||
			basename "$d" > /sys/bus/dax/drivers/kmem/new_id 2>/dev/null
		sleep 1
		D=$d; PN=$nid
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

# cram/nodes columns: 1 node 2 zratio 3 current 4 perceived 5 balloon 6 target
#                     7 blocked 8 allowed 9 free 10 present
nodes_col() { awk -v n="$PN" -v c="$1" '$1==n {print $c}' "$CRAM_DBG/nodes"; }
balloon() { nodes_col 5; }
blocked() { nodes_col 7; }

# poll FIELDFUNC CMP THRESH SECS
poll() {
	local fn=$1 cmp=$2 thr=$3 secs=$4 i v
	for ((i = 0; i < secs * 10; i++)); do
		v=$($fn)
		[ -n "$v" ] && [ "$v" "$cmp" "$thr" ] && return 0
		sleep 0.1
	done
	return 1
}

pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available (pass a raw drive to vng)"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
node_in_mask "$PN" has_private_memory ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online; control = $CTL"
trap 'echo "unblock $PN" > "$CTL" 2>/dev/null; echo "deflate $PN 4294967295" > "$CTL" 2>/dev/null; echo unplugged > "$D/state" 2>/dev/null; pn_restore_globals' EXIT

ktap_set_plan 6
dmesg -C 2>/dev/null
CHUNK=2048

# ---- Part A: each command's effect, on an (initially empty) node ----

# 1. inflate raises the balloon (empty node: reserves free pages, no reclaim)
b0=$(balloon)
echo "inflate $PN 8192" > "$CTL"
if poll balloon -ge $((b0 + 4096)) 10; then
	ktap_test_pass "inflate: balloon $b0 -> $(balloon)"
else
	ktap_test_fail "inflate did not raise balloon (was $b0, now $(balloon))"
fi

# 2. deflate lowers it again
echo "deflate $PN 4294967295" > "$CTL"
if poll balloon -le "$CHUNK" 10; then
	ktap_test_pass "deflate: balloon -> $(balloon)"
else
	ktap_test_fail "deflate did not lower balloon (now $(balloon))"
fi

# 3. block: cram/nodes reports blocked, and a demote is refused (spills to swap)
echo "block $PN" > "$CTL"
blk=$(blocked)
out=$("$GATE" "$PN"); rc=$?
if [ "$blk" = 1 ] && [ "$rc" = 3 ]; then
	ktap_test_pass "block: blocked=1, demotion refused [$out]"
elif [ "$rc" = 0 ]; then
	ktap_test_fail "block: demotion still landed on cram (blocked=$blk) [$out]"
else
	ktap_test_skip "block: could not stage demotion (blocked=$blk rc=$rc) [$out]"
fi

# 4. unblock: demotions resume (region lands on cram)
echo "unblock $PN" > "$CTL"
ublk=$(blocked)
out=$("$GATE" "$PN"); rc=$?
if [ "$ublk" = 0 ] && [ "$rc" = 0 ]; then
	ktap_test_pass "unblock: blocked=0, demotion resumes [$out]"
else
	ktap_test_fail "unblock did not resume demotion (blocked=$ublk rc=$rc) [$out]"
fi

# 5. clean slate
echo "deflate $PN 4294967295" > "$CTL"
if poll balloon -le "$CHUNK" 10; then
	ktap_test_pass "deflate-all restored balloon to $(balloon)"
else
	ktap_test_fail "balloon not drained (now $(balloon))"
fi

# ---- Part B: hammer the whole control surface during a demote/promote storm ----
# A background loop spams inflate/deflate/block/unblock while cram_pressure_tool
# runs its concurrent storm with a forward-progress watchdog.  We assert the box
# never wedges (storm rc != livelock) and nothing corrupts (clean dmesg); the
# storm legitimately may not make demote progress while blocking is toggled, so
# rc 3 (environmental "did not demote") is acceptable -- only rc 1 is a real fail.
(
	while :; do
		echo "inflate $PN 4096"          > "$CTL" 2>/dev/null
		echo "block $PN"                 > "$CTL" 2>/dev/null
		echo "deflate $PN 2048"          > "$CTL" 2>/dev/null
		echo "unblock $PN"               > "$CTL" 2>/dev/null
	done
) &
CTLPID=$!
"$STORM" "$PN" stress > /tmp/cram_control_storm.out 2>&1
rc=$?
kill "$CTLPID" 2>/dev/null; wait "$CTLPID" 2>/dev/null
echo "unblock $PN" > "$CTL" 2>/dev/null
echo "deflate $PN 4294967295" > "$CTL" 2>/dev/null
storm=$(tail -1 /tmp/cram_control_storm.out)
splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|WARNING:|Oops|refcount_t|modified in place|hung task")
if [ "$rc" = 1 ]; then
	ktap_test_fail "control storm wedged/leaked (rc=1) [$storm]"
elif [ "$splat" != 0 ]; then
	ktap_test_fail "splat during control storm (count=$splat) [$storm]"
else
	ktap_test_pass "control surface hammered under storm: no wedge/splat (rc=$rc) [$storm]"
fi

ktap_finished
