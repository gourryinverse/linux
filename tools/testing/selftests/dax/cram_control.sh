#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM provider control-surface test: exercises the cramdax sysfs signals and
# CRAM's state reporting, both for correctness and under placement pressure.
#
# Reuses cram_gate_tool (demote an anon region, report whether it landed on cram)
# and cram_pressure_tool (concurrent demote/promote storm with a livelock watchdog).
# Needs a swap device so a blocked demotion has somewhere to spill.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

GATE="$DIR/cram_gate_tool"
STORM="$DIR/cram_pressure_tool"

ktap_print_header
pn_require_root

[ -x "$GATE" ] && [ -x "$STORM" ] ||
	{ ktap_skip_all "cram_gate_tool / cram_pressure_tool not built"; exit "$KSFT_SKIP"; }

cram_provision() {
	local d nid r
	pn_modprobe nd_e820 dax_pmem device_dax nd_pmem
	modprobe -q cramdax 2>/dev/null
	[ -d /sys/bus/dax/drivers/cramdax ] ||
		{ ktap_skip_all "cramdax driver unavailable"; exit "$KSFT_SKIP"; }
	if command -v ndctl >/dev/null 2>&1; then
		for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
			ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f \
				>/dev/null 2>&1
		done
	fi
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node"); [ "$nid" -ge 0 ] 2>/dev/null || continue
		pn_bind_cramdax "$d" || continue
		sleep 1
		D=$d; PN=$nid
		[ -e "$D/balloon_target" ] && [ -e "$D/no_alloc" ] ||
			continue
		echo offline > "$D/state" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

balloon() { pn_node_vmstat "$PN" nr_balloon_pages; }

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
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online"
cleanup()
{
	echo 0 > "$D/no_alloc" 2>/dev/null
	echo 0 > "$D/balloon_target" 2>/dev/null
	echo offline > "$D/state" 2>/dev/null
	pn_restore_globals
}
trap cleanup EXIT

ktap_set_plan 6
dmesg -C 2>/dev/null
CHUNK=2048

# ---- Part A: each command's effect, on an (initially empty) node ----

# 1. inflate raises the balloon (empty node: reserves free pages, no reclaim)
b0=$(balloon)
echo $((b0 + 8192)) > "$D/balloon_target"
if poll balloon -ge $((b0 + 4096)) 10; then
	ktap_test_pass "inflate: balloon $b0 -> $(balloon)"
else
	ktap_test_fail "inflate did not raise balloon (was $b0, now $(balloon))"
fi

# 2. deflate lowers it again
echo 0 > "$D/balloon_target"
if poll balloon -le "$CHUNK" 10; then
	ktap_test_pass "deflate: balloon -> $(balloon)"
else
	ktap_test_fail "deflate did not lower balloon (now $(balloon))"
fi

# 3. allocation admission off: placement is refused and spills to swap
echo 1 > "$D/no_alloc"
out=$("$GATE" "$PN"); rc=$?
if [ "$rc" = 3 ]; then
	ktap_test_pass "no_alloc=1: placement refused [$out]"
elif [ "$rc" = 0 ]; then
	ktap_test_fail "no_alloc=1: placement still landed on CRAM [$out]"
else
	ktap_test_skip "no_alloc=1: could not stage placement (rc=$rc) [$out]"
fi

# 4. allocation admission on: placements resume
echo 0 > "$D/no_alloc"
out=$("$GATE" "$PN"); rc=$?
if [ "$rc" = 0 ]; then
	ktap_test_pass "no_alloc=0: placement resumes [$out]"
else
	ktap_test_fail "no_alloc=0 did not resume placement (rc=$rc) [$out]"
fi

# 5. clean slate
echo 0 > "$D/balloon_target"
if poll balloon -le "$CHUNK" 10; then
	ktap_test_pass "deflate-all restored balloon to $(balloon)"
else
	ktap_test_fail "balloon not drained (now $(balloon))"
fi

# ---- Part B: hammer the whole control surface during a demote/promote storm ----
# A background loop spams balloon targets and admission while cram_pressure_tool
# runs its concurrent storm with a forward-progress watchdog.  We assert the box
# never wedges (storm rc != livelock) and nothing corrupts (clean dmesg); the
# storm legitimately may not make demote progress while blocking is toggled, so
# rc 3 (environmental "did not demote") is acceptable -- only rc 1 is a real fail.
(
	while :; do
		echo 4096 > "$D/balloon_target" 2>/dev/null
		echo 1 > "$D/no_alloc" 2>/dev/null
		echo 2048 > "$D/balloon_target" 2>/dev/null
		echo 0 > "$D/no_alloc" 2>/dev/null
	done
) &
CTLPID=$!
"$STORM" "$PN" stress > /tmp/cram_control_storm.out 2>&1
rc=$?
kill "$CTLPID" 2>/dev/null; wait "$CTLPID" 2>/dev/null
echo 0 > "$D/no_alloc" 2>/dev/null
echo 0 > "$D/balloon_target" 2>/dev/null
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
