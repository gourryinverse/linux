#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate externally visible service exclusion for an anondax node.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TIER_BASE=/sys/devices/virtual/memory_tiering

ktap_print_header
pn_require_root

pn_modprobe nd_e820 dax_pmem device_dax nd_pmem anondax
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
	ktap_skip_all "no anondax-bindable device"
	exit "$KSFT_SKIP"
}

for node in "$NODE_BASE"/node[0-9]*; do
	nid=${node##*node}
	node_in_mask "$nid" has_common_memory || continue
	COMMON=$nid
	break
done

cleanup()
{
	echo "$(basename "$D")" > /sys/bus/dax/drivers/anondax/unbind 2>/dev/null
}
trap cleanup EXIT

tier_contains()
{
	local tier

	for tier in "$TIER_BASE"/memory_tier*/nodelist; do
		[ -r "$tier" ] || continue
		nodelist_has "$(cat "$tier")" "$1" && return 0
	done
	return 1
}

ktap_set_plan 4
dmesg -C 2>/dev/null

if [ ! -d "$TIER_BASE" ]; then
	ktap_test_skip "memory tiering unavailable"
	ktap_test_skip "memory tiering unavailable"
else
	if ! tier_contains "$PN"; then
		ktap_test_pass "private memory is excluded from demotion tiers"
	else
		ktap_test_fail "private node unexpectedly joined a memory tier"
	fi
	if [ -n "${COMMON:-}" ] && tier_contains "$COMMON"; then
		ktap_test_pass "common-memory tiering remains available"
	else
		ktap_test_skip "no observable common-memory tier control"
	fi
fi

pool=
for candidate in "$NODE_BASE/node$PN"/hugepages/hugepages-*; do
	[ -r "$candidate/nr_hugepages" ] || continue
	pool=$candidate/nr_hugepages
	break
done
if [ -z "$pool" ]; then
	ktap_test_skip "per-node HugeTLB pool unavailable"
else
	before=$(cat "$pool")
	echo $((before + 1)) > "$pool" 2>/dev/null
	after=$(cat "$pool")
	echo "$before" > "$pool" 2>/dev/null
	if [ "$after" = "$before" ]; then
		ktap_test_pass "HugeTLB pool placement excludes private memory"
	else
		ktap_test_fail "private-node HugeTLB pool grew $before->$after"
	fi
fi

splat=$(dmesg 2>/dev/null | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during service checks"
else
	ktap_test_fail "kernel splat during service checks (count=$splat)"
fi

ktap_finished
