#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate that generic folio walkers leave CRAM-resident anonymous memory alone.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/cram_exclusion_tool"

ktap_print_header
pn_require_root
[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_exclusion_tool not built"; exit "$KSFT_SKIP"; }

pn_modprobe nd_e820 dax_pmem device_dax nd_pmem cramdax
if command -v ndctl >/dev/null 2>&1; then
	for region in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
		ndctl create-namespace -m devdax \
			-e "${region/region/namespace}.0" -f >/dev/null 2>&1
	done
fi
for D in "$DAX_BASE"/dax*; do
	[ -e "$D/target_node" ] || continue
	PN=$(cat "$D/target_node")
	[ "$PN" -ge 0 ] 2>/dev/null || continue
	pn_bind_cramdax "$D" || continue
	echo online > "$D/state" 2>/dev/null || continue
	break
done
pn_node_is_private "${PN:--1}" || {
	ktap_skip_all "no online CRAM node"
	exit "$KSFT_SKIP"
}

cleanup()
{
	echo offline > "$D/state" 2>/dev/null
}
trap cleanup EXIT

ktap_set_plan 5
dmesg -C 2>/dev/null

out=$("$TOOL" collapse "$PN"); rc=$?
echo "$out"
if [ "$rc" = 3 ]; then
	ktap_test_skip "MADV_COLLAPSE prerequisites unavailable"
	ktap_test_skip "MADV_COLLAPSE prerequisites unavailable"
else
	grep -q 'collapse_excluded=1' <<<"$out" &&
		ktap_test_pass "MADV_COLLAPSE does not form a private-node THP" ||
		ktap_test_fail "MADV_COLLAPSE formed or accepted a private-node THP"
	grep -q 'collapse_stable=1' <<<"$out" &&
		ktap_test_pass "collapse leaves CRAM residency unchanged" ||
		ktap_test_fail "collapse relocated CRAM-resident pages"
fi

out=$("$TOOL" ksm "$PN"); rc=$?
echo "$out"
if [ "$rc" = 3 ]; then
	ktap_test_skip "KSM prerequisites unavailable"
	ktap_test_skip "KSM prerequisites unavailable"
else
	grep -q 'private_unmerged=1' <<<"$out" &&
		ktap_test_pass "KSM leaves private-node pages unmerged" ||
		ktap_test_fail "KSM merged private-node pages"
	grep -q 'common_merged=1' <<<"$out" &&
		ktap_test_pass "KSM common-memory control merged" ||
		ktap_test_fail "KSM control did not demonstrate merging"
fi

splat=$(dmesg 2>/dev/null | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during folio-walker checks"
else
	ktap_test_fail "kernel splat during folio-walker checks (count=$splat)"
fi

ktap_finished
