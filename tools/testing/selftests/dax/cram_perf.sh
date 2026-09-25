#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM-fault vs swap-fault latency benchmark.
#
# Provisions a CRAM private node and a physical swap device, then runs
# cram_perf_tool which measures per-page fault latency for, head to head on the
# same machine:
#   - cram-fault: first WRITE to an anon page demoted onto the CRAM node
#                 (present, read-only) -> cram->DRAM promote;
#   - swap-fault: first access to an anon page swapped to the physical swap
#                 device (NOT cram) -> swap-in.
# Both armed with the same MADV_PAGEOUT; the tool flips
# /sys/kernel/mm/numa/demotion_enabled per phase so swap-phase pages go straight
# to swap and never transit cram (it restores the knob on exit).  Each cell is
# reported sequential and random with mean/p50/p90/p99/min/max.
#
# This is a BENCHMARK, not a pass/fail correctness test: the RESULT lines are
# emitted to stdout for an orchestrator to scrape.  ktap_test_pass iff the tool
# ran (rc==0); skip on environmental rc==3.
#
# Provision a dax device on a memoryless node via memmap= (see vng.workflow).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

DEMOTION_KNOB=/sys/kernel/mm/numa/demotion_enabled
TOOL="$DIR/cram_perf_tool"

ktap_print_header
pn_require_root

[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_perf_tool not built"; exit "$KSFT_SKIP"; }

# Find a memoryless dax device, bind to cramdax, and online it.
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
		D=$d; DAX=$(basename "$d"); PN=$nid
		[ -e "$D/state" ] ||
			{ ktap_skip_all "$DAX has no CRAM state control"; exit "$KSFT_SKIP"; }
		echo offline > "$D/state" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node (see header)"
	exit "$KSFT_SKIP"
}

# Pure-swap baseline needs a real swap device for the demotion-off phase.
pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available (pass a raw drive to vng)"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online"

# Enable demotion so the cram phase actually demotes; the tool toggles it per
# phase and restores it, but seed it here and snapshot for restoration.
SAVED_DEMOTION=$(cat "$DEMOTION_KNOB" 2>/dev/null)
echo 1 > "$DEMOTION_KNOB" 2>/dev/null
trap '
	echo offline > "$D/state" 2>/dev/null
	[ -n "$SAVED_DEMOTION" ] && echo "$SAVED_DEMOTION" > "'"$DEMOTION_KNOB"'" 2>/dev/null
	pn_restore_globals
' EXIT

ktap_set_plan 1

# argv: <cram_nid> <no-alloc-path> [samples] [mono|tsc].
"$TOOL" "$PN" "$D/no_alloc" "${SAMPLES:-}" "${CLOCK:-mono}" \
	2>&1 | tee /tmp/cram_perf.out
rc=${PIPESTATUS[0]}

# Surface the scrapeable RESULT lines on stdout for the orchestrator.
grep '^RESULT ' /tmp/cram_perf.out

if [ "$rc" = 0 ]; then
	ktap_test_pass "cram-fault vs swap-fault benchmark completed (see RESULT lines / /tmp/cram_perf.out)"
elif [ "$rc" = 3 ]; then
	ktap_test_skip "could not establish cram demotion or swap (environmental)"
else
	ktap_test_fail "benchmark tool error (rc=$rc)"
fi

ktap_finished
