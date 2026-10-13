#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate userspace contracts for private kmem memory.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/kmem_private_policy_tool"
CGROOT=/sys/fs/cgroup
CG="$CGROOT/kmem-private-policy-$$"

ktap_print_header
pn_require_root
pn_require_test_device
[ -x "$TOOL" ] ||
	{ ktap_skip_all "kmem_private_policy_tool not built"; exit "$KSFT_SKIP"; }

pn_modprobe kmem

cleanup()
{
	[ -d "$CG" ] && rmdir "$CG" 2>/dev/null
	[ -z "${MEMCG_ENABLED:-}" ] ||
		echo -memory > "$CGROOT/cgroup.subtree_control" 2>/dev/null
	pn_unbind_kmem_private
}
trap cleanup EXIT

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
[ -n "${COMMON:-}" ] || {
	ktap_skip_all "no common memory node"
	exit "$KSFT_SKIP"
}
pn_require_mask_nodes "$PN" "$COMMON"

MEMLESS=
for ((nid = 0; nid < 64; nid++)); do
	nodelist_has "$(cat "$NODE_BASE/possible")" "$nid" || continue
	node_in_mask "$nid" has_memory && continue
	MEMLESS=$nid
	break
done

# The tool runs in a memcg for memcg_split_reclaimed when memcg v2 exists.
if [ "$(stat -fc %T "$CGROOT" 2>/dev/null)" = cgroup2fs ] &&
   grep -qw memory "$CGROOT/cgroup.controllers" 2>/dev/null; then
	if ! grep -qw memory "$CGROOT/cgroup.subtree_control" &&
	   echo +memory > "$CGROOT/cgroup.subtree_control" 2>/dev/null; then
		MEMCG_ENABLED=1
	fi
	mkdir "$CG" 2>/dev/null || CG=
else
	CG=
fi

ktap_set_plan 21
if [ -n "$CG" ]; then
	out=$(sh -ec "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' $PN $COMMON $MEMLESS")
else
	out=$("$TOOL" "$PN" "$COMMON" $MEMLESS)
fi
rc=$?
echo "$out"

for field in default_isolated mems_visible policy_rejected bind_accepted \
		mixed_narrowed unflagged_private_rejected private_flag_validated \
		target_allowed private_visible move_allowed lock_stable \
		pageout_acts private_flag_reported relative_task_common \
		relative_bind_common \
		static_mask_preserved preferred_private_rejected \
		hugetlb_private_rejected thp_split_reclaimed memcg_split_reclaimed \
		private_mask_exact; do
	if [ "$rc" = 0 ] && grep -qx "$field=1" <<<"$out"; then
		ktap_test_pass "$field"
	elif [ "$rc" = 0 ] && grep -qx "$field=2" <<<"$out"; then
		ktap_test_skip "$field (unavailable)"
	else
		ktap_test_fail "$field (rc=$rc)"
	fi
done

ktap_finished
