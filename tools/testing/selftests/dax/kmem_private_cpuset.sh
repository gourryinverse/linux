#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate cgroup-v2 cpuset behavior in the presence of private memory.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/kmem_private_policy_tool"
CGROOT=/sys/fs/cgroup
CG="$CGROOT/kmem-private-cpuset-$$"

ktap_print_header
pn_require_root
pn_require_test_device
[ -x "$TOOL" ] ||
	{ ktap_skip_all "kmem_private_policy_tool not built"; exit "$KSFT_SKIP"; }
[ "$(stat -fc %T "$CGROOT" 2>/dev/null)" = cgroup2fs ] ||
	{ ktap_skip_all "requires cgroup v2"; exit "$KSFT_SKIP"; }
grep -qw cpuset "$CGROOT/cgroup.controllers" 2>/dev/null ||
	{ ktap_skip_all "cpuset controller unavailable"; exit "$KSFT_SKIP"; }

pn_modprobe kmem

cleanup()
{
	rmdir "$CG" 2>/dev/null
	pn_disable_cpuset
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

pn_enable_cpuset || {
	ktap_skip_all "cannot enable cpuset controller"
	exit "$KSFT_SKIP"
}
mkdir "$CG" || exit 1
cat "$CGROOT/cpuset.cpus.effective" > "$CG/cpuset.cpus" || exit 1

ktap_set_plan 5
DMESG_START=$(dmesg 2>/dev/null | wc -l)

if echo "$COMMON,$PN" > "$CG/cpuset.mems" 2>/dev/null &&
	nodelist_has "$(cat "$CG/cpuset.mems.effective")" "$PN"; then
	ktap_test_pass "mixed common/private cpuset.mems is accepted"
else
	ktap_test_fail "mixed common/private cpuset.mems was not effective"
fi

if echo "$PN" > "$CG/cpuset.mems" 2>/dev/null &&
	nodelist_has "$(cat "$CG/cpuset.mems")" "$PN" &&
	nodelist_has "$(cat "$CG/cpuset.mems.effective")" "$PN" &&
	nodelist_has "$(cat "$CG/cpuset.mems.effective")" "$COMMON"; then
	ktap_test_pass "private request retains the node and inherits common fallback"
else
	ktap_test_fail "private-only request did not inherit safely"
fi

out=$(sh -ec "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' \
	'$PN' '$COMMON'")
if grep -q 'default_isolated=1' <<<"$out"; then
	ktap_test_pass "task attachment keeps ordinary allocation on common memory"
else
	ktap_test_fail "task attachment exposed private memory to ordinary allocation"
fi

if echo "$COMMON" > "$CG/cpuset.mems" 2>/dev/null &&
	! nodelist_has "$(cat "$CG/cpuset.mems.effective")" "$PN"; then
	ktap_test_pass "removing the private node revokes it"
else
	ktap_test_fail "private node remained effective after revocation"
fi

splat=$(dmesg 2>/dev/null | tail -n +$((DMESG_START + 1)) | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during cpuset transitions"
else
	ktap_test_fail "kernel splat during cpuset transitions (count=$splat)"
fi

ktap_finished
