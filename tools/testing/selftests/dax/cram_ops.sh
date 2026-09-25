#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM read-only anonymous-memory operations battery.
#
# Exercises core mm operations against resident present-read-only cram folios
# (mprotect, MADV_DONTNEED/FREE, mremap, mlock, process-exit leak, THP-PMD),
# verifying they behave like ordinary anon folios with no kernel splat.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/cram_ops_tool"
SUBTESTS="mprotect dontneed mremap madvfree mlock exit thp"

ktap_print_header
pn_require_root

[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_ops_tool not built"; exit "$KSFT_SKIP"; }

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

ktap_set_plan $(( $(echo $SUBTESTS | wc -w) + 1 ))

dmesg -C 2>/dev/null
for s in $SUBTESTS; do
	out=$("$TOOL" "$PN" "$s" 2>&1); rc=$?
	if [ "$rc" = 0 ]; then
		ktap_test_pass "$s: cram folio behaves as expected ($out)"
	elif [ "$rc" = 3 ]; then
		ktap_test_skip "$s: precondition not met ($out)"
	else
		ktap_test_fail "$s: rc=$rc ($out)"
	fi
done

splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:")
if [ "$splat" = 0 ]; then
	ktap_test_pass "no KASAN/BUG/UAF/WARN across the mm-ops battery"
else
	ktap_test_fail "kernel splat during mm-ops battery (count=$splat)"
fi

ktap_finished
