#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate policy behavior while a cgroup-v2 cpuset is reshaped.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/kmem_private_rebind_tool"
CGROOT=/sys/fs/cgroup
CG="$CGROOT/kmem-private-rebind-$$"
CGCOMMON="$CGROOT/kmem-private-rebind-common-$$"

ktap_print_header
pn_require_root
pn_require_test_device
[ -x "$TOOL" ] ||
	{ ktap_skip_all "kmem_private_rebind_tool not built"; exit "$KSFT_SKIP"; }
[ "$(stat -fc %T "$CGROOT" 2>/dev/null)" = cgroup2fs ] ||
	{ ktap_skip_all "requires cgroup v2"; exit "$KSFT_SKIP"; }

pn_modprobe kmem

cleanup()
{
	[ -n "${TARGET:-}" ] && kill "$TARGET" 2>/dev/null
	[ -n "${CHILD:-}" ] && kill "$CHILD" 2>/dev/null
	[ -n "${CHILD:-}" ] && wait "$CHILD" 2>/dev/null
	rm -f /tmp/kmem-private-rebind-$$.out
	rmdir "$CG" 2>/dev/null
	rmdir "$CGCOMMON" 2>/dev/null
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
pn_node_is_private "${PN:--1}" || {
	pn_skip_or_fail "no online kmem-private node"
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
pn_require_mask_nodes "$PN" "${COMMON[@]}"

pn_enable_cpuset || {
	ktap_skip_all "cannot enable cpuset controller"
	exit "$KSFT_SKIP"
}
mkdir "$CG" || exit 1
cat "$CGROOT/cpuset.cpus.effective" > "$CG/cpuset.cpus" || exit 1
echo "${COMMON[0]},$PN" > "$CG/cpuset.mems" || exit 1
mkdir "$CGCOMMON" || exit 1
cat "$CGROOT/cpuset.cpus.effective" > "$CGCOMMON/cpuset.cpus" || exit 1
echo "${COMMON[1]}" > "$CGCOMMON/cpuset.mems" || exit 1

run_case()
{
	local mode=$1 out=/tmp/kmem-private-rebind-$$.out i

	: > "$out"
	sh -ec "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' '$mode' '$PN' \
		'${COMMON[0]}' '${COMMON[1]}'" > "$out" 2>&1 &
	CHILD=$!
	for ((i = 0; i < 100; i++)); do
		grep -q '^ready$' "$out" && break
		kill -0 "$CHILD" 2>/dev/null || break
		sleep 0.1
	done
	if ! grep -q '^ready$' "$out" ||
	   ! echo "${COMMON[1]},$PN" > "$CG/cpuset.mems"; then
		stop_case
		return 1
	fi
	kill -USR1 "$CHILD"
	wait "$CHILD" || return 1
	CHILD=
	tail -1 "$out"
}

wait_output()
{
	local pattern=$1 out=/tmp/kmem-private-rebind-$$.out i

	for ((i = 0; i < 100; i++)); do
		grep -q "$pattern" "$out" && return 0
		kill -0 "$CHILD" 2>/dev/null || break
		sleep 0.1
	done
	return 1
}

stop_case()
{
	[ -n "${TARGET:-}" ] && kill "$TARGET" 2>/dev/null
	[ -n "${CHILD:-}" ] && kill "$CHILD" 2>/dev/null
	[ -n "${CHILD:-}" ] && wait "$CHILD" 2>/dev/null
	TARGET=
	CHILD=
}

transition_cpuset()
{
	local action=$1 stage=$2 mask=$3

	case "$action:$stage" in
	attach:1) echo "$TARGET" > "$CGCOMMON/cgroup.procs" ;;
	attach:*) echo "$TARGET" > "$CG/cgroup.procs" ;;
	*) echo "$mask" > "$CG/cpuset.mems" ;;
	esac
}

run_private_transitions()
{
	local mode=$1 label=$2 out=/tmp/kmem-private-rebind-$$.out
	local action=${3:-reshape}
	local stage mask title result
	local titles=("keeps private pages and placement while the node stays allowed"
		"evacuates private pages and uses common placement when access is revoked"
		"restores the original private request when access returns")

	echo "${COMMON[0]},$PN" > "$CG/cpuset.mems"
	if [ "$action" = attach ]; then
		echo "${COMMON[1]},$PN" > "$CG/cpuset.mems"
	fi
	: > "$out"
	if [ "$mode" = 3 ] && [ "$action" = reshape ]; then
		timeout 60 sh -ec "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' \
			'$mode' '$PN' '${COMMON[0]}' '${COMMON[1]}'" > "$out" 2>&1 &
	else
		# Shared-policy parents and task-attachment helpers start at root.
		timeout 60 "$TOOL" "$mode" "$PN" "${COMMON[0]}" "${COMMON[1]}" \
			> "$out" 2>&1 &
	fi
	CHILD=$!
	if ! wait_output '^ready pid='; then
		for title in "${titles[@]}"; do
			ktap_test_fail "$label $title (helper setup failed)"
		done
		stop_case
		return
	fi
	TARGET=$(sed -n 's/^ready pid=//p' "$out")
	if [ "$mode" = 4 ] && [ "$action" = reshape ]; then
		echo "$TARGET" > "$CG/cgroup.procs"
	fi
	for stage in 0 1 2; do
		mask=${COMMON[1]}
		[ "$stage" = 1 ] || mask+=",$PN"
		if transition_cpuset "$action" "$stage" "$mask" &&
		   kill -USR1 "$TARGET" &&
		   wait_output "^stage=$stage "; then
			result=$(grep "^stage=$stage " "$out")
			echo "$result"
			if grep -q 'ok=1$' <<<"$result"; then
				ktap_test_pass "$label ${titles[$stage]}"
			else
				ktap_test_fail "$label ${titles[$stage]}"
			fi
		else
			ktap_test_fail "$label ${titles[$stage]} (helper failed or timed out)"
		fi
	done
	wait "$CHILD"
	CHILD=
	TARGET=
}

run_private_retype()
{
	local mode=$1 label=$2 out=/tmp/kmem-private-rebind-$$.out
	local stage title result rc ready=false
	local titles=("falls back while the private node is offline"
		"binds the original node when it returns as common memory"
		"falls back again after draining the common node"
		"restores private placement when the node returns as private memory")

	echo "${COMMON[1]},$PN" > "$CG/cpuset.mems"
	: > "$out"
	timeout 60 sh -ec "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' \
		'$mode' '$PN' '${COMMON[0]}' '${COMMON[1]}'" > "$out" 2>&1 &
	CHILD=$!
	if wait_output '^ready pid='; then
		TARGET=$(sed -n 's/^ready pid=//p' "$out")
		ready=true
	fi
	for stage in 0 1 2 3; do
		title="$label ${titles[$stage]}"
		if [ "$ready" != true ]; then
			ktap_test_fail "$title (helper setup failed)"
			continue
		fi
		case "$stage" in
		0|2) echo unplugged > "$D/state" ;;
		1) echo 0 > "$D/private" && echo online_movable > "$D/state" ;;
		3) echo 1 > "$D/private" && echo online_movable > "$D/state" ;;
		esac
		rc=$?
		if [ "$rc" = 0 ] && kill -USR1 "$TARGET" &&
		   wait_output "^stage=$stage "; then
			result=$(grep "^stage=$stage " "$out")
			echo "$result"
			if grep -q 'ok=1$' <<<"$result"; then
				ktap_test_pass "$title"
			else
				ktap_test_fail "$title"
			fi
		else
			ktap_test_fail "$title (hotplug or helper failed)"
			ready=false
		fi
	done
	stop_case
	pn_bind_kmem_private "$D"
}

run_common_retype()
{
	local mode=$1 label=$2 out=/tmp/kmem-private-rebind-$$.out
	local result

	if ! echo unplugged > "$D/state" || ! echo 0 > "$D/private" ||
	   ! echo online_movable > "$D/state"; then
		ktap_test_fail "$label (cannot provision common kmem memory)"
		return
	fi
	echo "${COMMON[0]},$PN" > "$CG/cpuset.mems"
	: > "$out"
	timeout 60 sh -ec "echo \$\$ > '$CG/cgroup.procs'; exec '$TOOL' \
		'$mode' '$PN' '${COMMON[0]}' '${COMMON[1]}'" > "$out" 2>&1 &
	CHILD=$!
	if ! wait_output '^ready pid='; then
		ktap_test_fail "$label (helper setup failed)"
		stop_case
		return
	fi
	TARGET=$(sed -n 's/^ready pid=//p' "$out")
	if ! echo unplugged > "$D/state" || ! echo 1 > "$D/private" ||
	   ! echo online_movable > "$D/state" ||
	   ! echo "${COMMON[1]},$PN" > "$CG/cpuset.mems"; then
		ktap_test_fail "$label (cannot retype kmem memory)"
		stop_case
		return
	fi
	kill -USR1 "$TARGET"
	if wait "$CHILD"; then
		result=$(tail -1 "$out")
		echo "$result"
		if grep -q 'ok=1$' <<<"$result"; then
			ktap_test_pass "$label"
		else
			ktap_test_fail "$label [$result]"
		fi
	else
		ktap_test_fail "$label (helper failed or timed out)"
	fi
	CHILD=
	TARGET=
}

ktap_set_plan 26
DMESG_START=$(dmesg 2>/dev/null | wc -l)

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
	ktap_test_fail "removed common-node pages did not migrate [$out]"
fi

run_private_transitions 3 "anonymous binding"
run_private_transitions 4 "shared binding across distinct mapper cpusets"
run_private_transitions 3 "anonymous binding across task attachment" attach
run_private_transitions 4 "shared binding across task attachment" attach
run_private_retype 7 "anonymous private binding"
run_private_retype 8 "shared private binding"
run_common_retype 5 "shared common binding falls back after its node becomes private"
run_common_retype 6 "shared common binding requires an allowed common target"

splat=$(dmesg 2>/dev/null | tail -n +$((DMESG_START + 1)) | grep -ciE \
	'KASAN|BUG:|WARNING:|Oops|use-after-free|refcount_t')
if [ "$splat" = 0 ]; then
	ktap_test_pass "no kernel splat during policy rebind"
else
	ktap_test_fail "kernel splat during policy rebind (count=$splat)"
fi

ktap_finished
