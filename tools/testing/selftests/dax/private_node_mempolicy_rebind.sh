#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# MPOL_BIND policy rebind across cpuset.mems changes (mpol_rebind_mm).
#
#   D1 default bind({D1}): drop D1 from mems then re-add -> remapped onto D0,
#      NOT restored (a default MPOL_BIND has its pol->nodes remapped positionally
#      and forgotten -- w.cpuset_mems_allowed tracks the shrunk mems).  This is
#      the flags=0 remap, distinct from MPOL_F_RELATIVE_NODES which re-derives
#      from the stored user positions (see private_node_mempolicy_relative.sh).
#   D2 static   bind({D1}): drop D1 then re-add -> RESTORED (MPOL_F_STATIC_NODES
#      keeps the original node in w.user_nodemask).
#   D3 a public bind that cpuset remaps onto an opted-in (CAP_USER_NUMA) private
#      node stays functional: mpol_rebind sets MPOL_F_PRIVATE, so fresh faults
#      reach the private node through the private zonelist instead of livelocking
#      on the FALLBACK zonelist (which excludes it).
#
# Needs two public (DRAM) nodes plus the opted-in private node; SKIPs otherwise.
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

pn_set user_numa 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN as private (user_numa)"
	pn_reset; exit "$KSFT_SKIP"
fi

CPUS=$(cat "$CG/cpuset.cpus.effective")
DRAM=$(pn_dram_list); set -- $DRAM; D0=$1; D1=${2:-}
ktap_print_msg "private node $PN; DRAM={$DRAM} D0=$D0 D1=${D1:-none} cpus=$CPUS"
# D1/D2 are public-node remaps; D3 remaps a public bind onto an opted-in
# private node (which IS in cpuset.mems under the USER_NUMA model).
ktap_set_plan 3

HF=/tmp/pn_reb_h.$$
start_holder() {	# CG MODE NID MB HOLD -> HPID HADDR HJOB
	: > "$HF"
	( echo $BASHPID > "$1/cgroup.procs"; exec "$TOOL" mbindhold "$2" "$3" "$4" "$5" ) >"$HF" 2>&1 &
	HJOB=$!
	for _ in $(seq 1 30); do grep -q "pid=" "$HF" && break; sleep 0.2; done
	HPID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$HF" | head -1)
	HADDR=$(sed -n 's/.*addr=\(0x[0-9a-f]*\).*/\1/p' "$HF" | head -1)
}
mkchild() { mkdir -p "$1" 2>/dev/null; echo "$CPUS" > "$1/cpuset.cpus" 2>/dev/null; }
reap() { echo $$ > "$CG/cgroup.procs" 2>/dev/null; kill -9 "${HJOB:-0}" 2>/dev/null; wait "${HJOB:-0}" 2>/dev/null; rmdir "$1" 2>/dev/null; }
# polnodes ADDR-policy "bind:..." -> the nodelist after the colon
polnodes() { echo "${1#*:}"; }

# rebind_flap MODE -> echoes "restore_policy=<nodelist>"; drops then re-adds D1
rebind_flap() {
	local g="$CG/reb_$1"; mkchild "$g"
	echo "$D0,$D1" > "$g/cpuset.mems" 2>/dev/null
	start_holder "$g" "$1" "$D1" 16 40
	sed 's/^/# /' "$HF"
	local p0 onD1_0
	p0=$(nm_policy "$HPID" "$HADDR"); onD1_0=$(nm_on_node "$HPID" "$HADDR" "$D1")
	echo "$D0" > "$g/cpuset.mems" 2>/dev/null; sleep 2		# drop D1
	local p1 onD1_1 onD0_1
	p1=$(nm_policy "$HPID" "$HADDR")
	onD1_1=$(nm_on_node "$HPID" "$HADDR" "$D1"); onD0_1=$(nm_on_node "$HPID" "$HADDR" "$D0")
	echo "$D0,$D1" > "$g/cpuset.mems" 2>/dev/null; sleep 1		# re-add D1
	local p2; p2=$(nm_policy "$HPID" "$HADDR")
	# residency-after-drop tells us whether NORMAL-node pages are actively
	# migrated by the cpuset.mems write (contrast with the private-node case D3).
	ktap_print_msg "$1: pol $p0->$p1->$p2 ; onD1 $onD1_0->$onD1_1 onD0_afterDrop=$onD0_1 (normal-node migrate evidence)"
	REST=$(polnodes "$p2")
	reap "$g"
}

if [ -z "$D1" ]; then
	ktap_test_skip "D1 needs two public nodes (have {$DRAM})"
	ktap_test_skip "D2 needs two public nodes"
	ktap_test_skip "D3 needs two public nodes"
else
	# --- D1: default-remap bind is NOT restored -------------------------------
	rebind_flap bind
	if ! nodelist_has "${REST:-x}" "$D1"; then
		ktap_test_pass "D1 default bind({$D1}) remapped onto $D0 and NOT restored on re-add (pol={$REST})"
	else
		ktap_test_fail "D1 default bind unexpectedly restored $D1 (pol={$REST})"
	fi
	# --- D2: static bind IS restored ------------------------------------------
	rebind_flap bindstatic
	if nodelist_has "${REST:-x}" "$D1"; then
		ktap_test_pass "D2 static bind({$D1}) RESTORED to $D1 when re-added (pol={$REST})"
	else
		ktap_test_fail "D2 static bind not restored on re-add (pol={$REST})"
	fi
	# --- D3: public bind remapped onto the private node stays functional -------
	# mbindrefault faults half the range (phase1), holds while we remap D1->PN,
	# then faults fresh pages (phase2).  Without the MPOL_F_PRIVATE rebind fixup
	# phase2 livelocks on the FALLBACK zonelist; with it the fresh faults reach
	# the private node and phase2 completes.
	g="$CG/reb_priv"; mkchild "$g"
	echo "$D0,$D1" > "$g/cpuset.mems" 2>/dev/null
	: > "$HF"
	( echo $BASHPID > "$g/cgroup.procs"; exec "$TOOL" mbindrefault "$D1" 16 8 ) >"$HF" 2>&1 &
	HJOB=$!
	for _ in $(seq 1 30); do grep -q phase1 "$HF" && break; sleep 0.2; done
	echo "$D0,$PN" > "$g/cpuset.mems" 2>/dev/null		# remap D1 -> private PN
	for _ in $(seq 1 40); do grep -q phase2 "$HF" && break; sleep 0.5; done
	sed 's/^/# /' "$HF"
	if grep -q phase2 "$HF"; then
		ktap_test_pass "D3 public bind remapped onto private $PN stays functional (fresh faults completed)"
	else
		ktap_test_fail "D3 fresh faults livelocked after remap onto private $PN (MPOL_F_PRIVATE not set on rebind)"
	fi
	reap "$g"
fi

rm -f "$HF"
pn_reset
ktap_finished
