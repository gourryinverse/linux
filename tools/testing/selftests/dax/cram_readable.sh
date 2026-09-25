#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM read-only anonymous-memory round-trip test.
#
# Pages anonymous memory out so the reclaim path demotes it onto the CRAM
# private node by migration, and verifies it lands present read-only on the
# node (zero-copy reads) and COW-promotes back to DRAM on write, with no splat.
#
# Provision a dax device on a memoryless node via memmap= (see vng.workflow).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/cram_readable_tool"

ktap_print_header
pn_require_root

[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_readable_tool not built"; exit "$KSFT_SKIP"; }

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

# MADV_PAGEOUT needs swap active so reclaim engages; cram diverts anon folios
# before the swap step, so they land on cram while swap is the fallback.
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
"$TOOL" "$PN" 2>&1 | tee /tmp/cram_readable.out
rc=${PIPESTATUS[0]}
out=$(cat /tmp/cram_readable.out)
splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:")

# 1. read-only round trip: place present-RO on CRAM, zero-copy read, COW-promote
if [ "$rc" = 0 ]; then
	ktap_test_pass "read-only CRAM: place present-RO on node $PN, read zero-copy, write promoted"
elif [ "$rc" = 3 ]; then
	ktap_test_skip "anon did not demote to CRAM (environmental): $out"
else
	ktap_test_fail "read-only CRAM contract failed (rc=$rc): $out"
fi

# 2. no kernel splat during the round trip
if [ "$splat" = 0 ]; then
	ktap_test_pass "no KASAN/BUG/UAF/WARN during read-only CRAM round trip"
else
	ktap_test_fail "kernel splat during read-only CRAM round trip (count=$splat)"
fi

ktap_finished
