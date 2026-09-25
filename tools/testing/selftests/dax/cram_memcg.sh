#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate private-memory accounting through cgroup v2.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/cram_hold_tool"
CGROOT=/sys/fs/cgroup
CG="$CGROOT/cram-memcg-$$"
OUT=/tmp/cram-memcg-$$.out

ktap_print_header
pn_require_root
[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_hold_tool not built"; exit "$KSFT_SKIP"; }
[ "$(stat -fc %T "$CGROOT" 2>/dev/null)" = cgroup2fs ] ||
	{ ktap_skip_all "requires cgroup v2"; exit "$KSFT_SKIP"; }
grep -qw memory "$CGROOT/cgroup.controllers" 2>/dev/null ||
	{ ktap_skip_all "memory controller unavailable"; exit "$KSFT_SKIP"; }

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
	[ -n "${CHILD:-}" ] && kill "$CHILD" 2>/dev/null
	[ -n "${CHILD:-}" ] && wait "$CHILD" 2>/dev/null
	rmdir "$CG" 2>/dev/null
	rm -f "$OUT"
	echo offline > "$D/state" 2>/dev/null
}
trap cleanup EXIT

echo +memory > "$CGROOT/cgroup.subtree_control" 2>/dev/null || {
	ktap_skip_all "cannot enable memory controller"
	exit "$KSFT_SKIP"
}
mkdir "$CG" || exit 1

sh -c "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' '$PN'" > "$OUT" 2>&1 &
CHILD=$!
for ((i = 0; i < 100; i++)); do
	grep -q '^ready ' "$OUT" 2>/dev/null && break
	kill -0 "$CHILD" 2>/dev/null || break
	sleep 0.1
done
grep -q '^ready ' "$OUT" 2>/dev/null || {
	ktap_skip_all "could not place the memcg workload on CRAM"
	exit "$KSFT_SKIP"
}
ktap_set_plan 3
dmesg -C 2>/dev/null
current=$(cat "$CG/memory.current")
if [ "$current" -ge $((8 * 1024 * 1024)) ]; then
	ktap_test_pass "memory.current charges the private allocation"
else
	ktap_test_fail "private allocation missing from memory.current ($current)"
fi

bytes=$(awk -v key="N$PN=" '$1 == "anon" {
	for (i = 2; i <= NF; i++) if ($i ~ ("^" key)) { split($i, a, "="); print a[2] }
}' "$CG/memory.numa_stat")
if [ "${bytes:-0}" -ge $((8 * 1024 * 1024)) ]; then
	ktap_test_pass "memory.numa_stat accounts private-node anonymous memory"
else
	ktap_test_fail "private-node charge missing from memory.numa_stat ($bytes)"
fi

splat=$(dmesg 2>/dev/null | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during memcg accounting"
else
	ktap_test_fail "kernel splat during memcg accounting (count=$splat)"
fi

ktap_finished
