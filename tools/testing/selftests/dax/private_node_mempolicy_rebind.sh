#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# MPOL_BIND policy rebind across cpuset.mems changes (mpol_rebind_mm).
#
#   D1 relative bind({D1}): drop D1 from mems then re-add -> folded onto D0,
#      NOT restored (relative nodes are remapped positionally and forgotten).
#   D2 static   bind({D1}): drop D1 then re-add -> RESTORED (MPOL_F_STATIC_NODES
#      keeps the original node in w.user_nodemask).
#
# Needs two public (DRAM) nodes; private bind is absolute (cpuset default-open),
# covered by private_node_cpuset_default_open.sh.  See private_node_common.sh.

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
pn_set hotunplug 1
pn_hotplug online_movable
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online node $PN as private (user_numa)"
	pn_reset; exit "$KSFT_SKIP"
fi

CPUS=$(cat "$CG/cpuset.cpus.effective")
DRAM=$(pn_dram_list); set -- $DRAM; D0=$1; D1=${2:-}
ktap_print_msg "private node $PN; DRAM={$DRAM} D0=$D0 D1=${D1:-none} cpus=$CPUS"
# Public-node rebind only (private nodes are never in cpuset.mems, so never
# positionally remapped).
ktap_set_plan 2

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
else
	# --- D1: relative bind is NOT restored ------------------------------------
	rebind_flap bind
	if ! nodelist_has "${REST:-x}" "$D1"; then
		ktap_test_pass "D1 relative bind({$D1}) folded onto $D0 and NOT restored on re-add (pol={$REST})"
	else
		ktap_test_fail "D1 relative bind unexpectedly restored $D1 (pol={$REST})"
	fi
	# --- D2: static bind IS restored ------------------------------------------
	rebind_flap bindstatic
	if nodelist_has "${REST:-x}" "$D1"; then
		ktap_test_pass "D2 static bind({$D1}) RESTORED to $D1 when re-added (pol={$REST})"
	else
		ktap_test_fail "D2 static bind not restored on re-add (pol={$REST})"
	fi
fi

rm -f "$HF"
pn_reset
ktap_finished
