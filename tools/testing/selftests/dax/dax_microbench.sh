#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Tiering microbenchmark runner (perf, not a pass/fail regression).
#
# Drives dax_microbench_tool with a hot working set sized above the top tier so
# the kernel demotes to the lower tier (a kmem-bound dax node) and then to swap,
# and reports per-cell throughput / latency / demotion+swap vmstat deltas.  The 4
# cells are {anon,file} x {ro,rw}; each is a KTAP "test" that passes if it ran.
#
# Requires: a kmem-bound dax node (demotion target) + swap on.  Provision e.g.:
#   memmap=4G!8G ; daxctl reconfigure-device -N -m system-ram dax0.0
#   daxctl online-memory dax0.0   (online_movable) ; swapon <dev>
#
# Env: DAX_MB_SECS (per-cell, default 60), DAX_MB_SIZE_MB (default overflows
#      node0+dax into swap), DAX_MB_PATTERN (rand|seq, default rand),
#      DAX_MB_FS_DIR (real FS dir for the file cells; file cells SKIP if unset).

DIR="$(dirname "$(readlink -f "$0")")"
# shellcheck disable=SC1091
. "$DIR"/../kselftest/ktap_helpers.sh

TOOL="$DIR/dax_microbench_tool"
DAX_BASE=/sys/bus/dax/devices
SECS=${DAX_MB_SECS:-60}
PATTERN=${DAX_MB_PATTERN:-rand}
FS_DIR=${DAX_MB_FS_DIR:-}

node_free_mb() { awk '/MemFree/{print int($4/1024)}' "/sys/devices/system/node/node$1/meminfo"; }

is_kmem_dax() {
	local drv
	[ -e "$DAX_BASE/$1/state" ] || return 1
	drv=$(readlink "$DAX_BASE/$1/driver" 2>/dev/null)
	[ "$(basename "${drv:-}")" = kmem ]
}
find_kmem_dax_node() {
	local d
	for d in "$DAX_BASE"/dax*; do
		is_kmem_dax "$(basename "$d")" || continue
		cat "$d"/target_node 2>/dev/null && return 0
	done
	return 1
}

ktap_print_header
ktap_set_plan 4

[ -x "$TOOL" ] || { ktap_skip_all "dax_microbench_tool not built"; exit "$KSFT_SKIP"; }
[ "$(awk '/SwapTotal/{print $2}' /proc/meminfo)" -gt 0 ] || {
	ktap_skip_all "no swap configured"; exit "$KSFT_SKIP"; }

DAX_NODE=$(find_kmem_dax_node)
[ -n "$DAX_NODE" ] || { ktap_skip_all "no kmem-bound dax node (demotion target)"; exit "$KSFT_SKIP"; }

N0_FREE=$(node_free_mb 0)
DAX_FREE=$(node_free_mb "$DAX_NODE")
SIZE=${DAX_MB_SIZE_MB:-$(( N0_FREE + DAX_FREE + 1024 ))}

BIND=""
command -v numactl >/dev/null 2>&1 && BIND="numactl --cpunodebind=0 --membind=0"

ktap_print_msg "tier: node0(free ${N0_FREE}MB) -> dax node${DAX_NODE}(free ${DAX_FREE}MB) -> swap"
ktap_print_msg "workingset=${SIZE}MB pattern=${PATTERN} secs=${SECS} bind='${BIND:-none}'"

run_cell() { # <label> <mode> <rw> [path]
	local label=$1 mode=$2 rw=$3 path=$4
	echo "# --- cell: $label ---"
	if $BIND "$TOOL" "$mode" "$rw" "$SIZE" "$SECS" "$PATTERN" $path; then
		ktap_test_pass "$label"
	else
		ktap_test_fail "$label (tool rc=$?)"
	fi
}

run_cell "anon-ro" anon ro
run_cell "anon-rw" anon rw
if [ -n "$FS_DIR" ] && [ -d "$FS_DIR" ]; then
	run_cell "file-ro" file ro "$FS_DIR/dax_mb.dat"
	run_cell "file-rw" file rw "$FS_DIR/dax_mb.dat"
else
	ktap_test_skip "file-ro (set DAX_MB_FS_DIR to a real FS)"
	ktap_test_skip "file-rw (set DAX_MB_FS_DIR to a real FS)"
fi

ktap_finished
