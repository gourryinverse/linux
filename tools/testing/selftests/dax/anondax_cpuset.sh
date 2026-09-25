#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate cgroup-v2 cpuset behavior in the presence of private memory.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/anondax_policy_tool"
CGROOT=/sys/fs/cgroup
CG="$CGROOT/anondax-cpuset-$$"

ktap_print_header
pn_require_root
[ -x "$TOOL" ] ||
	{ ktap_skip_all "anondax_policy_tool not built"; exit "$KSFT_SKIP"; }
[ "$(stat -fc %T "$CGROOT" 2>/dev/null)" = cgroup2fs ] ||
	{ ktap_skip_all "requires cgroup v2"; exit "$KSFT_SKIP"; }
grep -qw cpuset "$CGROOT/cgroup.controllers" 2>/dev/null ||
	{ ktap_skip_all "cpuset controller unavailable"; exit "$KSFT_SKIP"; }

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
[ -n "${COMMON:-}" ] || {
	ktap_skip_all "no common memory node"
	exit "$KSFT_SKIP"
}

cleanup()
{
	rmdir "$CG" 2>/dev/null
	echo "$(basename "$D")" > /sys/bus/dax/drivers/anondax/unbind 2>/dev/null
}
trap cleanup EXIT

echo +cpuset > "$CGROOT/cgroup.subtree_control" 2>/dev/null || {
	ktap_skip_all "cannot enable cpuset controller"
	exit "$KSFT_SKIP"
}
mkdir "$CG" || exit 1
cat "$CGROOT/cpuset.cpus.effective" > "$CG/cpuset.cpus"

ktap_set_plan 5
dmesg -C 2>/dev/null

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

out=$(sh -c "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' \
	'/dev/$(basename "$D")' '$PN' '$COMMON'")
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

splat=$(dmesg 2>/dev/null | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during cpuset transitions"
else
	ktap_test_fail "kernel splat during cpuset transitions (count=$splat)"
fi

ktap_finished
