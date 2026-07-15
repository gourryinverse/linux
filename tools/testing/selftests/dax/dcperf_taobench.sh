#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# dcperf_taobench.sh - DCPerf / TaoBench tier-overflow runner (Meta-infra lane).
#
# NOT portable: requires the Benchpress harness (`fbpkg fetch cea.chips.benchpress`)
# and is excluded from any sanitized/published suite (see plan section 7).  Wraps
# a single TaoBench run confined below the top tier so the resident set overflows
# node0 into the demote tier, brackets it with collect_stats, and scrapes QPS.
#
# TaoBench is memory-capacity-bound (hit-ratio ~0.9, resident set ~all hot), so a
# footprint above node0 capacity cleanly exercises demote -> (dax/cram/swap).
#
# Env:
#   BP_DIR      benchpress checkout/fetch dir (has ./benchpress)   [required]
#   MEMSIZE     TaoBench footprint GB (set ABOVE node0 capacity)   [default 30]
#   TEST_TIME   seconds                                            [default 720]
#   MEM_MAX     cgroup2 memory.max to force node0->tier demotion   [optional]
#   OUT         results/snapshots dir                              [default /tmp/dcperf]
#
# Prereqs the caller must have set (plan 4b.3): iommu=pt boot arg,
# ulimit -n 1000000, chef-off/turbo-on, swap ON.

set -u
DIR="$(dirname "$(readlink -f "$0")")"
CS="$DIR/collect_stats.sh"

BP_DIR=${BP_DIR:?set BP_DIR to the benchpress dir}
MEMSIZE=${MEMSIZE:-30}
TEST_TIME=${TEST_TIME:-720}
WARMUP=$(( 10 * MEMSIZE )); [ "$WARMUP" -lt 1200 ] && WARMUP=1200
OUT=${OUT:-/tmp/dcperf}
mkdir -p "$OUT"

[ -x "$BP_DIR/benchpress" ] || { echo "no benchpress at $BP_DIR" >&2; exit 4; }
[ "$(awk '/SwapTotal/{print $2}' /proc/meminfo)" -gt 0 ] || { echo "swap must be ON" >&2; exit 4; }

run() { echo "+ $*"; "$@"; }

# Optional cgroup2 confinement so node0 overflows into the tier.
SCOPE=()
if [ -n "${MEM_MAX:-}" ]; then
	SCOPE=(systemd-run --scope -p MemoryMax="$MEM_MAX" -p MemorySwapMax=max)
fi

bash "$CS" snapshot before "$OUT/snap" >/dev/null

# bind_mem=0 is mandatory for CXL: the default binds each instance to local DRAM
# and never touches the tier.
run "${SCOPE[@]}" "$BP_DIR/benchpress" run tao_bench_standalone \
	-i "{\"memsize\":$MEMSIZE,\"bind_mem\":0,\"warmup_time\":$WARMUP,\"test_time\":$TEST_TIME}" \
	-k perf | tee "$OUT/taobench.log"

bash "$CS" snapshot after "$OUT/snap" >/dev/null

echo "=== tier counter deltas over the run ==="
bash "$CS" delta "$OUT/snap/before" "$OUT/snap/after" | tee "$OUT/deltas.txt"

echo "=== topline QPS ==="
m=$(ls -t benchmark_metrics_*/tao_bench_*_metrics_*_iter_None.json 2>/dev/null | head -1)
if [ -n "$m" ] && command -v jq >/dev/null 2>&1; then
	jq '{total_qps:.metrics.total_qps, fast_qps:.metrics.fast_qps,
	     slow_qps:.metrics.slow_qps, hit_ratio:.metrics.hit_ratio,
	     score:.metrics.score}' "$m" | tee "$OUT/qps.json"
else
	echo "metrics json not found or jq missing (looked for benchmark_metrics_*/...)" >&2
fi
