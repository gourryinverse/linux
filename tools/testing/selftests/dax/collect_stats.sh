#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# collect_stats.sh - snapshot and diff the memory-tiering counters that the
# CRAM / private-node test plan cares about.  Portable (no Meta
# infra); safe to run in a VM or on a host.  Used by the perf runners to bracket
# a benchmark and by the analysis step to produce per-cell deltas.
#
# Usage:
#   collect_stats.sh snapshot <name> [dir]   # dump all counters into dir/<name>
#   collect_stats.sh delta   <before> <after># numeric diff of two snapshots
#   collect_stats.sh report  <snapshot>      # print the headline counters
#
# A "snapshot" is a directory of raw counter files.  delta/report parse the
# vmstat-style "key value" files within, so they work offline on saved runs.
#
# Sources captured (each optional - absent files are simply skipped):
#   /proc/vmstat /proc/meminfo /proc/pressure/{cpu,memory,io} /proc/swaps
#   per-node /sys/devices/system/node/node*/{vmstat,meminfo,numastat}
#   numastat(8) if present
#   /sys/kernel/debug/cram/{demote_count,promote_count,promote_fail,nodes}
#   /sys/kernel/debug/zswap/*
#   a memcg dir via $CS_MEMCG (memory.stat/numa_stat/pressure/current/swap.current)

set -u

# Counters surfaced by "report" and diffed loudly by "delta" (from plan section 5).
CS_KEYS="
pgdemote_kswapd pgdemote_direct pgdemote_khugepaged
pgdemote_swap_fallback
pgpromote_success pgpromote_candidate
pgscan_kswapd pgscan_direct pgsteal_kswapd pgsteal_direct
workingset_refault_anon workingset_refault_file
pswpin pswpout zswpin zswpout zswpwb
allocstall_normal allocstall_movable allocstall_dma32
pgmigrate_success pgmigrate_fail
numa_hint_faults numa_pages_migrated
"

cs_ts() { cat /proc/uptime 2>/dev/null | cut -d' ' -f1; }   # monotonic, no wall clock needed

cs_snapshot() {
	local name="$1" dir="${2:-/tmp/cram_stats}"
	local out="$dir/$name"
	mkdir -p "$out" || { echo "collect_stats: cannot mkdir $out" >&2; return 1; }

	cs_ts > "$out/uptime"
	for f in /proc/vmstat /proc/meminfo /proc/swaps \
		 /proc/pressure/cpu /proc/pressure/memory /proc/pressure/io; do
		[ -r "$f" ] && cp "$f" "$out/$(echo "${f#/proc/}" | tr / _)" 2>/dev/null
	done

	# Per-node counters.
	local n nid
	for n in /sys/devices/system/node/node[0-9]*; do
		[ -d "$n" ] || continue
		nid=$(basename "$n")
		[ -r "$n/vmstat" ]   && cp "$n/vmstat"   "$out/${nid}_vmstat"   2>/dev/null
		[ -r "$n/meminfo" ]  && cp "$n/meminfo"  "$out/${nid}_meminfo"  2>/dev/null
		[ -r "$n/numastat" ] && cp "$n/numastat" "$out/${nid}_numastat" 2>/dev/null
	done
	command -v numastat >/dev/null 2>&1 && numastat -m > "$out/numastat_m" 2>/dev/null

	# CRAM debugfs (only present on cram builds with a cram node).
	local c=/sys/kernel/debug/cram
	if [ -d "$c" ]; then
		local f
		for f in demote_count promote_count promote_fail nodes; do
			[ -r "$c/$f" ] && cp "$c/$f" "$out/cram_$f" 2>/dev/null
		done
	fi

	# zswap debugfs.
	if [ -d /sys/kernel/debug/zswap ]; then
		local f b
		for f in /sys/kernel/debug/zswap/*; do
			[ -r "$f" ] || continue
			b=$(basename "$f")
			printf '%s %s\n' "$b" "$(cat "$f" 2>/dev/null)" >> "$out/zswap"
		done
	fi

	# Optional memcg.
	if [ -n "${CS_MEMCG:-}" ] && [ -d "$CS_MEMCG" ]; then
		local f
		for f in memory.stat memory.numa_stat memory.pressure \
			 memory.current memory.swap.current; do
			[ -r "$CS_MEMCG/$f" ] && cp "$CS_MEMCG/$f" "$out/memcg_$f" 2>/dev/null
		done
	fi

	echo "$out"
}

# Print "key<TAB>after-before" for CS_KEYS present in both /proc/vmstat dumps,
# plus cram debugfs and swap deltas.
cs_delta() {
	local before="$1" after="$2" k bv av
	[ -d "$before" ] && [ -d "$after" ] || {
		echo "collect_stats: delta needs two snapshot dirs" >&2; return 1; }

	local bt at
	bt=$(cat "$before/uptime" 2>/dev/null); at=$(cat "$after/uptime" 2>/dev/null)
	[ -n "$bt" ] && [ -n "$at" ] &&
		printf 'elapsed_s\t%s\n' "$(awk "BEGIN{printf \"%.2f\", $at-$bt}")"

	for k in $CS_KEYS; do
		bv=$(awk -v k="$k" '$1==k{print $2}' "$before/vmstat" 2>/dev/null)
		av=$(awk -v k="$k" '$1==k{print $2}' "$after/vmstat" 2>/dev/null)
		[ -n "$bv" ] && [ -n "$av" ] || continue
		[ "$av" = "$bv" ] && continue
		printf '%s\t%s\n' "$k" "$((av - bv))"
	done

	# CRAM debugfs single-value counters.
	for k in demote_count promote_count promote_fail; do
		bv=$(cat "$before/cram_$k" 2>/dev/null); av=$(cat "$after/cram_$k" 2>/dev/null)
		[ -n "$bv" ] && [ -n "$av" ] && [ "$av" != "$bv" ] &&
			printf 'cram_%s\t%s\n' "$k" "$((av - bv))"
	done
}

# Human-readable one-shot of the headline counters in a single snapshot.
cs_report() {
	local snap="$1" k v
	[ -d "$snap" ] || { echo "collect_stats: no snapshot $snap" >&2; return 1; }
	for k in $CS_KEYS; do
		v=$(awk -v k="$k" '$1==k{print $2}' "$snap/vmstat" 2>/dev/null)
		[ -n "$v" ] && printf '%-26s %s\n' "$k" "$v"
	done
	for k in demote_count promote_count promote_fail; do
		v=$(cat "$snap/cram_$k" 2>/dev/null)
		[ -n "$v" ] && printf '%-26s %s\n' "cram_$k" "$v"
	done
	[ -r "$snap/cram_nodes" ] && { echo "-- cram nodes --"; cat "$snap/cram_nodes"; }
}

case "${1:-}" in
	snapshot) shift; cs_snapshot "$@" ;;
	delta)    shift; cs_delta "$@" ;;
	report)   shift; cs_report "$@" ;;
	*) echo "usage: $0 {snapshot <name> [dir] | delta <before> <after> | report <snap>}" >&2
	   exit 2 ;;
esac
