#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate the MM services that private kmem memory includes and excludes.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TIER_BASE=/sys/devices/virtual/memory_tiering

ktap_print_header
pn_require_root
pn_require_test_device

pn_modprobe kmem
trap pn_unbind_kmem_private EXIT

for D in "$DAX_BASE"/dax*; do
	[ "$DAX_KMEM_TEST_DEV" = auto ] ||
		[ "$(basename "$D")" = "$DAX_KMEM_TEST_DEV" ] || continue
	[ -e "$D/target_node" ] || continue
	PN=$(cat "$D/target_node")
	[ "$PN" -ge 0 ] 2>/dev/null || continue
	pn_bind_kmem_private "$D" || continue
	break
done
[ -n "${D:-}" ] && [ "$(cat "$D/private" 2>/dev/null)" = 1 ] &&
pn_is_private || {
	pn_skip_or_fail "no private kmem device"
}

for node in "$NODE_BASE"/node[0-9]*; do
	nid=${node##*node}
	node_in_mask "$nid" has_common_memory || continue
	COMMON=$nid
	break
done


tier_contains()
{
	local tier

	for tier in "$TIER_BASE"/memory_tier*/nodelist; do
		[ -r "$tier" ] || continue
		nodelist_has "$(cat "$tier")" "$1" && return 0
	done
	return 1
}

ktap_set_plan 7
DMESG_START=$(dmesg 2>/dev/null | wc -l)

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

compact="$NODE_BASE/node$PN/compact"
if ! pgrep -x "kswapd$PN" >/dev/null; then
	ktap_test_fail "kswapd$PN is not running"
elif [ ! -e "$compact" ]; then
	ktap_test_skip "kcompactd unavailable without compaction"
elif pgrep -x "kcompactd$PN" >/dev/null; then
	ktap_test_pass "kswapd and kcompactd run for the private node"
else
	ktap_test_fail "kcompactd$PN is not running"
fi

if [ ! -e "$compact" ]; then
	ktap_test_skip "per-node compaction unavailable"
elif echo 1 > "$compact" 2>/dev/null; then
	ktap_test_pass "private-node compaction is accepted"
else
	ktap_test_fail "private-node compaction was rejected"
fi

# An idle node may have nothing to reclaim, which reports EAGAIN.
reclaim="$NODE_BASE/node$PN/reclaim"
if [ ! -e "$reclaim" ]; then
	ktap_test_skip "per-node reclaim unavailable"
elif err=$( (echo 1M > "$reclaim") 2>&1 ) ||
     grep -q "Resource temporarily unavailable" <<<"$err"; then
	ktap_test_pass "private-node reclaim is accepted"
else
	ktap_test_fail "private-node reclaim was rejected [$err]"
fi

splat=$(dmesg 2>/dev/null | tail -n +$((DMESG_START + 1)) | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during service checks"
else
	ktap_test_fail "kernel splat during service checks (count=$splat)"
fi

ktap_finished
