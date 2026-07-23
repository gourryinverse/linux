#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# The >=1-fallback cpuset.mems rule for private nodes.
#
# cpuset.mems governs all nodes, private included, but a non-empty set with NO
# fallback (public) node cannot serve allocations that are ineligible for a
# private node -- everything except an explicit MPOL_F_PRIVATE bind, plus all
# unmovable/kernel allocations.  A private node is absent from the default
# (FALLBACK) zonelist, so such a set is free-but-unreachable memory: a hardwall
# allocation finds no zone, the OOM path declines (there is free movable memory
# elsewhere), and the fault refaults -> wedge.  This is the node analogue of a
# ZONE_MOVABLE-only cpuset, so we reject it, the same way.
#
#   W1 a private-only cpuset.mems write is rejected (-EINVAL).
#   W2 a {public, private} cpuset.mems write is accepted.
#   W3 nested cpusets whose mems_allowed intersect down to a private-only
#      effective set do NOT strand a task: the effective mask inherits the
#      parent (which always has a fallback), and a no-policy allocation in the
#      leaf completes on a public node instead of wedging.
#
# W1/W2 need one private + one public node; W3 needs one private + two public.
# See private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision
pn_reset
CG=$(pn_cgroup2) || { ktap_skip_all "cgroup2 cpuset unavailable"; pn_reset; exit "$KSFT_SKIP"; }

# PN device-owned private (in N_MEMORY, not a fallback node).
pn_set private 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN as a private node"
	pn_reset; exit "$KSFT_SKIP"
fi

DRAM=$(pn_dram_list); set -- $DRAM; D0=$1; D1=${2:-}
CPUS=$(cat "$CG/cpuset.cpus.effective")
ktap_print_msg "private node $PN; DRAM={$DRAM} D0=$D0 D1=${D1:-none} cpus=$CPUS"
ktap_set_plan 3

mkchild() { mkdir -p "$1" 2>/dev/null; echo "$CPUS" > "$1/cpuset.cpus" 2>/dev/null; }
rmchild() { rmdir "$1" 2>/dev/null; }

# --- W1: private-only cpuset.mems is rejected -----------------------------
g="$CG/fb_w1"; mkchild "$g"
if echo "$PN" > "$g/cpuset.mems" 2>/dev/null; then
	ktap_test_fail "W1 private-only cpuset.mems={$PN} was accepted (got {$(cat "$g/cpuset.mems")})"
else
	ktap_test_pass "W1 private-only cpuset.mems={$PN} rejected (-EINVAL)"
fi
rmchild "$g"

# --- W2: {public, private} cpuset.mems is accepted ------------------------
g="$CG/fb_w2"; mkchild "$g"
if echo "$D0,$PN" > "$g/cpuset.mems" 2>/dev/null && \
   nodelist_has "$(cat "$g/cpuset.mems")" "$PN"; then
	ktap_test_pass "W2 cpuset.mems={$D0,$PN} accepted (has a fallback node)"
else
	ktap_test_fail "W2 cpuset.mems={$D0,$PN} unexpectedly rejected"
fi
rmchild "$g"

# --- W3: nested disjoint-fallback intersection does not strand a task ------
if [ -z "$D1" ]; then
	ktap_test_skip "W3 needs two public nodes (have {$DRAM})"
else
	P="$CG/fb_w3p"; C="$P/leaf"
	mkchild "$P"; echo "+cpuset" > "$P/cgroup.subtree_control" 2>/dev/null
	echo "$D0,$PN" > "$P/cpuset.mems" 2>/dev/null		# parent: fallback D0
	mkchild "$C"; echo "$D1,$PN" > "$C/cpuset.mems" 2>/dev/null	# leaf wants D1,PN
	# leaf mems_allowed & parent effective = {PN} (fallback-less) -> must inherit
	# the parent's effective mask so a fallback node survives.
	EFF=$(cat "$C/cpuset.mems.effective" 2>/dev/null)
	fbok=0; for n in $(tr ',' ' ' <<<"$EFF"); do pn_node_is_public "${n%-*}" && fbok=1; done
	HF=/tmp/pn_fb_h.$$; : > "$HF"
	( echo $BASHPID > "$C/cgroup.procs"; exec "$TOOL" mbindhold none 0 16 6 ) >"$HF" 2>&1 &
	HJOB=$!
	for _ in $(seq 1 40); do grep -q "addr=" "$HF" && break; sleep 0.2; done
	sed 's/^/# /' "$HF"
	HPID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$HF" | head -1)
	HADDR=$(sed -n 's/.*addr=\(0x[0-9a-f]*\).*/\1/p' "$HF" | head -1)
	faulted=0; [ -n "$HADDR" ] && faulted=1
	onpriv=$(nm_on_node "${HPID:-0}" "${HADDR:-0}" "$PN")
	ktap_print_msg "W3 leaf effective={$EFF} fallback_present=$fbok faulted=$faulted onPN=${onpriv:-?}"
	echo $$ > "$CG/cgroup.procs" 2>/dev/null; kill -9 "${HJOB:-0}" 2>/dev/null; wait "${HJOB:-0}" 2>/dev/null
	if [ "$fbok" = 1 ] && [ "$faulted" = 1 ] && [ "${onpriv:-0}" -eq 0 ] 2>/dev/null; then
		ktap_test_pass "W3 fallback-less leaf inherited a fallback; no-policy alloc completed off-private"
	else
		ktap_test_fail "W3 leaf stranded (fallback_present=$fbok faulted=$faulted onPN=$onpriv)"
	fi
	rm -f "$HF"; rmchild "$C"; rmchild "$P"
fi

pn_reset
ktap_finished
