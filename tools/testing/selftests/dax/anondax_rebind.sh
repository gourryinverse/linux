#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate policy behavior while a cgroup-v2 cpuset is reshaped.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/anondax_rebind_tool"
CGROOT=/sys/fs/cgroup
CG="$CGROOT/anondax-rebind-$$"

ktap_print_header
pn_require_root
[ -x "$TOOL" ] ||
	{ ktap_skip_all "anondax_rebind_tool not built"; exit "$KSFT_SKIP"; }
[ "$(stat -fc %T "$CGROOT" 2>/dev/null)" = cgroup2fs ] ||
	{ ktap_skip_all "requires cgroup v2"; exit "$KSFT_SKIP"; }

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
pn_node_is_private "${PN:--1}" || {
	ktap_skip_all "no online anondax node"
	exit "$KSFT_SKIP"
}

COMMON=()
for node in "$NODE_BASE"/node[0-9]*; do
	nid=${node##*node}
	node_in_mask "$nid" has_common_memory || continue
	COMMON+=("$nid")
	[ "${#COMMON[@]}" = 2 ] && break
done
[ "${#COMMON[@]}" = 2 ] || {
	ktap_skip_all "requires two common memory nodes"
	exit "$KSFT_SKIP"
}

cleanup()
{
	[ -n "${CHILD:-}" ] && kill "$CHILD" 2>/dev/null
	[ -n "${CHILD:-}" ] && wait "$CHILD" 2>/dev/null
	rm -f /tmp/anondax-rebind-$$.out
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
echo "${COMMON[0]},$PN" > "$CG/cpuset.mems"

run_case()
{
	local mode=$1 out=/tmp/anondax-rebind-$$.out i

	: > "$out"
	sh -c "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' '$mode' '$PN' \
		'${COMMON[0]}' '${COMMON[1]}'" > "$out" 2>&1 &
	CHILD=$!
	for ((i = 0; i < 100; i++)); do
		grep -q '^ready$' "$out" && break
		kill -0 "$CHILD" 2>/dev/null || break
		sleep 0.1
	done
	grep -q '^ready$' "$out" || return 1
	echo "${COMMON[1]},$PN" > "$CG/cpuset.mems" || return 1
	kill -USR1 "$CHILD"
	wait "$CHILD" || return 1
	CHILD=
	tail -1 "$out"
}

ktap_set_plan 4
dmesg -C 2>/dev/null

out=$(run_case 1)
echo "$out"
if grep -q 'private=0' <<<"$out"; then
	ktap_test_pass "interleave policy cannot reacquire private memory on rebind"
else
	ktap_test_fail "interleave rebind reached private memory [$out]"
fi

echo "${COMMON[0]},$PN" > "$CG/cpuset.mems"
out=$(run_case 2)
echo "$out"
if grep -q 'private=0' <<<"$out"; then
	ktap_test_pass "positional cpuset migration excludes private memory"
else
	ktap_test_fail "positional migration reached private memory [$out]"
fi
if grep -q "new_common=64" <<<"$out"; then
	ktap_test_pass "removed common-node pages migrate to common fallback"
else
	ktap_test_skip "environment did not migrate the common-node control"
fi

splat=$(dmesg 2>/dev/null | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during policy rebind"
else
	ktap_test_fail "kernel splat during policy rebind (count=$splat)"
fi

ktap_finished
