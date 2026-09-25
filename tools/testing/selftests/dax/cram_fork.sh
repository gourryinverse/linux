#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM read-only anonymous-memory fork / multi-mapper scaling test.
#
# Demotes anonymous pages onto the CRAM node, forks many children sharing those
# present read-only cram folios, and verifies shared zero-copy reads plus
# per-mapper COW-promote isolation, with no kernel splat.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/cram_fork_tool"

ktap_print_header
pn_require_root

[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_fork_tool not built"; exit "$KSFT_SKIP"; }

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

pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available (pass a raw drive to vng)"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online"
trap 'echo offline > "$D/state" 2>/dev/null; pn_restore_globals' EXIT

ktap_set_plan 2

dmesg -C 2>/dev/null
"$TOOL" "$PN" 32 2>&1 | tee /tmp/cram_fork.out
rc=${PIPESTATUS[0]}
out=$(cat /tmp/cram_fork.out)
splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:")

# 1. shared zero-copy reads + per-mapper COW isolation across many forks
if [ "$rc" = 0 ]; then
	ktap_test_pass "fork scaling: 32 mappers share cram RO, writes COW-isolate, parent intact"
elif [ "$rc" = 3 ]; then
	ktap_test_skip "region did not demote to CRAM (environmental): $out"
else
	ktap_test_fail "fork/multi-mapper contract failed (rc=$rc): $out"
fi

# 2. no kernel splat (refcount/UAF guard under the fan-out)
if [ "$splat" = 0 ]; then
	ktap_test_pass "no KASAN/BUG/UAF/WARN during fork fan-out"
else
	ktap_test_fail "kernel splat during fork fan-out (count=$splat)"
fi

ktap_finished
