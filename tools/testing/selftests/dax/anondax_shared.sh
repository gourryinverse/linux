#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Test the explicit MAP_SHARED allocation interface of anondax.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/anondax_shared_tool"

ktap_print_header
pn_require_root

[ -x "$TOOL" ] ||
	{ ktap_skip_all "anondax_shared_tool not built"; exit "$KSFT_SKIP"; }

pn_modprobe nd_e820 dax_pmem device_dax nd_pmem anondax
[ -d /sys/bus/dax/drivers/anondax ] ||
	{ ktap_skip_all "anondax driver unavailable"; exit "$KSFT_SKIP"; }

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
	pn_bind_anondax "$D" || continue
	break
done

[ -n "${D:-}" ] && [ "$(basename "$(readlink "$D/driver")")" = anondax ] || {
	ktap_skip_all "no anondax-bindable device on a memoryless node"
	exit "$KSFT_SKIP"
}

DEV=/dev/$(basename "$D")
cleanup()
{
	echo "$(basename "$D")" > /sys/bus/dax/drivers/anondax/unbind 2>/dev/null
}
trap cleanup EXIT

ktap_set_plan 7
dmesg -C 2>/dev/null

if pn_node_is_private "$PN"; then
	ktap_test_pass "anondax onlined node $PN as private memory"
else
	ktap_test_fail "anondax node $PN is not private"
fi

out=$("$TOOL" "$DEV" "$PN"); rc=$?
echo "$out"

if grep -q 'private_rejected=1' <<<"$out"; then
	ktap_test_pass "MAP_PRIVATE is rejected"
else
	ktap_test_fail "MAP_PRIVATE was accepted"
fi
if grep -q 'shared=1' <<<"$out"; then
	ktap_test_pass "mappings of one open file share pages"
else
	ktap_test_fail "same-file mappings did not share data"
fi
if grep -q 'isolated=1' <<<"$out"; then
	ktap_test_pass "separate opens create independent objects"
else
	ktap_test_fail "separate opens unexpectedly shared data"
fi
if [ "$rc" = 0 ] && grep -q 'fork_shared=1' <<<"$out" &&
	grep -q 'on_node=64' <<<"$out"; then
	ktap_test_pass "shared faults and fork stay on private node $PN"
else
	ktap_test_fail "shared placement contract failed (rc=$rc)"
fi

name=$(basename "$D")
echo "$name" > /sys/bus/dax/drivers/anondax/unbind 2>/dev/null
if ! node_in_mask "$PN" has_memory; then
	ktap_test_pass "unmapped anondax memory hot-removed on unbind"
else
	ktap_test_fail "anondax memory remained after clean unbind"
fi
trap - EXIT

splat=$(dmesg 2>/dev/null | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during anondax lifecycle"
else
	ktap_test_fail "kernel splat during anondax lifecycle (count=$splat)"
fi

ktap_finished
