#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Tier latency probe runner (perf, not a pass/fail regression).
#
# Characterises the tiers before the throughput microbench:
#   - access  latency per memory node (DRAM node0 vs CXL/dax nodes) via
#     pointer-chase, printing the DRAM-vs-tier gap
#   - re-fault latency with zswap OFF (= swap fault) and ON (= zswap fault)
# Each probe is a KTAP "test" that passes if it ran.
#
# Env: LAT_MB (access buf, default 256), LAT_STEPS_M (default 100),
#      LAT_REFAULT_MB (default 128).

DIR="$(dirname "$(readlink -f "$0")")"
# shellcheck disable=SC1091
. "$DIR"/../kselftest/ktap_helpers.sh

TOOL="$DIR/tier_latency_tool"
MB=${LAT_MB:-256}
STEPS=${LAT_STEPS_M:-100}
RMB=${LAT_REFAULT_MB:-128}
ZSWAP=/sys/module/zswap/parameters/enabled

mem_nodes() { for d in /sys/devices/system/node/node[0-9]*; do
	[ "$(awk '/MemTotal/{print $4}' "$d/meminfo")" -gt 0 ] && basename "$d" | tr -dc 0-9 && echo; done; }

# A private node is not probeable this way, and the two failure modes both look
# like a broken probe rather than a node that declined:
#
#   no USER_NUMA          numactl's mbind() is refused outright, rc=1
#   movable-only          the membind is process-wide, so the task's UNMOVABLE
#                         allocations are confined to a node that has no zone
#                         for them, and it is OOM-killed (rc=137)
#
# Both are the kernel behaving correctly.  Probe public nodes; say why the
# others were left out rather than reporting a failure.
node_is_public() {
	local l r a b IFS=,
	l=$(cat /sys/devices/system/node/has_public_memory 2>/dev/null) || return 1
	for r in $l; do
		a=${r%%-*}; b=${r##*-}
		[ -n "$a" ] || continue
		[ "$1" -ge "$a" ] && [ "$1" -le "$b" ] && return 0
	done
	return 1
}

ktap_print_header
NODES=$(mem_nodes)
NNODE=$(echo "$NODES" | wc -w)
ktap_set_plan $((NNODE + 2))

[ -x "$TOOL" ] || { ktap_skip_all "tier_latency_tool not built"; exit "$KSFT_SKIP"; }
command -v numactl >/dev/null 2>&1 || ktap_print_msg "numactl absent: access probe not node-bound"

# --- access latency per node ---
for n in $NODES; do
	echo "# --- access: node$n ---"
	if ! node_is_public "$n"; then
		ktap_test_skip "access node$n (private: a process-wide membind either is refused or OOMs)"
		continue
	fi
	BIND=""; command -v numactl >/dev/null 2>&1 && BIND="numactl --cpunodebind=0 --membind=$n"
	if $BIND "$TOOL" access "$MB" "$STEPS"; then
		ktap_test_pass "access node$n"
	else
		ktap_test_fail "access node$n (rc=$?)"
	fi
done

# --- refault latency: swap vs zswap ---
if [ "$(awk '/SwapTotal/{print $2}' /proc/meminfo)" -gt 0 ]; then
	saved=""; [ -w "$ZSWAP" ] && saved=$(cat "$ZSWAP")
	[ -w "$ZSWAP" ] && echo N > "$ZSWAP"
	echo "# --- refault: zswap OFF (swap) ---"
	"$TOOL" refault "$RMB" && ktap_test_pass "refault swap" || ktap_test_fail "refault swap (rc=$?)"

	if [ -w "$ZSWAP" ]; then
		echo Y > "$ZSWAP"
		echo "# --- refault: zswap ON ---"
		"$TOOL" refault "$RMB" && ktap_test_pass "refault zswap" || ktap_test_fail "refault zswap (rc=$?)"
		echo "$saved" > "$ZSWAP"
	else
		ktap_test_skip "refault zswap (zswap knob not writable)"
	fi
else
	ktap_test_skip "refault swap (no swap)"
	ktap_test_skip "refault zswap (no swap)"
fi

ktap_finished
