#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# MPOL_F_RELATIVE_NODES under the governed private-node model.
#
# A relative bind stores the user nodemask as a set of POSITIONS (not absolute
# node ids).  At set time -- and again on every cpuset.mems rebind -- the kernel
# folds those positions onto the userland-NUMA (USER_NUMA) nodes of the current
# cpuset: the public nodes plus any private node that opted into CAP_USER_NUMA.
# Device-owned (non-USER_NUMA) private nodes are NOT part of the position space,
# even when they are present in cpuset.mems.  This must be consistent between
# mpol_set_nodemask() (create) and mpol_rebind_nodemask() (cpuset change).
#
#   R1 set-time fold lands positionally within the public position space
#      (position N -> the Nth USER_NUMA node of the cpuset, folded mod count --
#      the bit index is a position, not a node id).
#   R2 set-time fold can reach an opted-in (USER_NUMA) private node; the pages
#      are placed there (MPOL_F_PRIVATE routes through the private zonelist).
#   R3 set-time fold SKIPS a device-owned (non-USER_NUMA) private node that is
#      in cpuset.mems -- it does not count as a position.
#   R4 a cpuset.mems shrink RE-FOLDS the stored positions over the NEW USER_NUMA
#      position space.  Relative "follows" the cpuset: positions are re-derived
#      from w.user_nodemask, not the previous pol->nodes remapped.  A position
#      whose bit index is not itself a USER_NUMA node id must still fold (this is
#      the case that a positions-as-node-ids bug would wrongly empty).
#
# Needs two public (DRAM) nodes plus one private node; SKIPs otherwise.
# See private_node_common.sh.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

PN_CAP_USER_NUMA=$((1 << 8))	# NODE_MEMORY_CAP_USER_NUMA, nodeN/mem_features bit 8

pn_begin
pn_require_tool
pn_provision
pn_reset
CG=$(pn_cgroup2) || { ktap_skip_all "cgroup2 cpuset unavailable"; pn_reset; exit "$KSFT_SKIP"; }

DRAM=$(pn_dram_list); set -- $DRAM; D0=$1; D1=${2:-}
CPUS=$(cat "$CG/cpuset.cpus.effective")
if [ -z "$D1" ]; then
	ktap_set_plan 4
	for r in R1 R2 R3 R4; do ktap_test_skip "$r needs two public nodes (have {$DRAM})"; done
	pn_reset; ktap_finished; exit 0
fi
ktap_print_msg "private node $PN; DRAM={$DRAM} D0=$D0 D1=$D1 cpus=$CPUS"
ktap_set_plan 4

# --- helpers --------------------------------------------------------------
# expand_nl "0,2-3" -> "0\n2\n3"
expand_nl() {
	local tok lo hi n
	for tok in $(echo "$1" | tr ',' ' '); do
		lo=${tok%-*}; hi=${tok#*-}
		for n in $(seq "$lo" "$hi"); do echo "$n"; done
	done
}
# node_is_usernuma NID -- true if the node opts CAP_USER_NUMA (public nodes do
# too: FALLBACK => all caps).
node_is_usernuma() {
	local mf; mf=$(cat "$NODE_BASE/node$1/mem_features" 2>/dev/null) || return 1
	[ $(( ${mf:-0} & PN_CAP_USER_NUMA )) -ne 0 ]
}
# usernuma_sorted "memslist" -- ascending USER_NUMA node ids within that mems set.
usernuma_sorted() {
	local n out=
	for n in $(expand_nl "$1"); do node_is_usernuma "$n" && out="$out $n"; done
	echo $out | tr ' ' '\n' | sort -n | tr '\n' ' '
}
# foldpos POS space... -- the element at (POS mod count): the kernel's relative fold.
foldpos() { local pos=$1; shift; local a=("$@") w=$#; [ "$w" -gt 0 ] && echo "${a[$((pos % w))]}"; }
# index_of VAL list... -- 0-based index of VAL in the (already sorted) list, or -1.
index_of() { local v=$1; shift; local i=0 n; for n in "$@"; do [ "$n" = "$v" ] && { echo "$i"; return; }; i=$((i+1)); done; echo -1; }

HF=/tmp/pn_rel_h.$$
start_holder() {	# CG MODE POS MB HOLD -> HPID HADDR
	: > "$HF"
	( echo $BASHPID > "$1/cgroup.procs"; exec "$TOOL" mbindhold "$2" "$3" "$4" "$5" ) >"$HF" 2>&1 &
	HJOB=$!
	for _ in $(seq 1 30); do grep -q "addr=" "$HF" && break; sleep 0.2; done
	HPID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$HF" | head -1)
	HADDR=$(sed -n 's/.*addr=\(0x[0-9a-f]*\).*/\1/p' "$HF" | head -1)
}
mkchild() { mkdir -p "$1" 2>/dev/null; echo "$CPUS" > "$1/cpuset.cpus" 2>/dev/null; }
reap() { echo $$ > "$CG/cgroup.procs" 2>/dev/null; kill -9 "${HJOB:-0}" 2>/dev/null; wait "${HJOB:-0}" 2>/dev/null; rmdir "$1" 2>/dev/null; }
polnodes() { echo "${1#*:}"; }

online_pn() {	# online_pn <user_numa 0|1> -- bring PN up private, opting user_numa or not
	pn_reset
	pn_set private 1
	pn_set user_numa "$1"
	pn_hotplug online_movable
	{ [ "$(pn_state)" = online_movable ] && pn_is_private; }
}

# ========================================================================
# R1-R2, R4 use PN as an opted-in USER_NUMA private node.
if ! online_pn 1 || ! node_is_usernuma "$PN"; then
	ktap_test_skip "R1 could not online $PN as USER_NUMA private"
	ktap_test_skip "R2 could not online $PN as USER_NUMA private"
else
	# --- R1: set-time positional fold in the public position space -----
	g="$CG/rel_r1"; mkchild "$g"; echo "$D0,$D1" > "$g/cpuset.mems"
	SP=$(usernuma_sorted "$D0,$D1"); POS=1		# bit 1 = 2nd position, not "node 1"
	EXP=$(foldpos "$POS" $SP)
	start_holder "$g" bindrelative "$POS" 16 40
	sed 's/^/# /' "$HF"
	ONEXP=$(nm_on_node "$HPID" "$HADDR" "$EXP"); POL=$(nm_policy "$HPID" "$HADDR")
	ktap_print_msg "R1 space={$SP} pos=$POS -> expect node $EXP ; pol=$POL onExp=$ONEXP"
	if [ "${ONEXP:-0}" -gt 0 ] 2>/dev/null && ! nodelist_has "$(polnodes "$POL")" "$PN"; then
		ktap_test_pass "R1 relative pos $POS folded onto public node $EXP (positional, not node id)"
	else
		ktap_test_fail "R1 relative pos $POS did not land on expected public node $EXP (pol=$POL)"
	fi
	reap "$g"

	# --- R2: set-time fold reaches an opted-in private node -----------
	g="$CG/rel_r2"; mkchild "$g"; echo "$D0,$D1,$PN" > "$g/cpuset.mems"
	SP=$(usernuma_sorted "$D0,$D1,$PN")		# includes PN (USER_NUMA)
	POS=$(index_of "$PN" $SP)			# position that folds onto PN
	start_holder "$g" bindrelative "$POS" 16 40
	sed 's/^/# /' "$HF"
	ONPN=$(nm_on_node "$HPID" "$HADDR" "$PN"); POL=$(nm_policy "$HPID" "$HADDR")
	ktap_print_msg "R2 space={$SP} pos=$POS -> expect private $PN ; pol=$POL onPN=$ONPN"
	if [ "${ONPN:-0}" -gt 0 ] 2>/dev/null; then
		ktap_test_pass "R2 relative pos $POS reached opted-in private $PN (pages placed there)"
	else
		ktap_test_fail "R2 relative fold did not reach private $PN (pol=$POL onPN=$ONPN)"
	fi
	reap "$g"

	# --- R4: cpuset.mems shrink re-folds over the NEW position space ---
	# pos=8: bit 8 is not a node id in this topology, so a bug that treats the
	# positions as node ids (rel &= USER_NUMA) would empty the fold and reset to
	# fallback instead of re-deriving the position.
	g="$CG/rel_r4"; mkchild "$g"; echo "$D0,$D1,$PN" > "$g/cpuset.mems"
	POS=8
	SPB=$(usernuma_sorted "$D0,$D1,$PN"); EXPB=$(foldpos "$POS" $SPB)
	start_holder "$g" bindrelative "$POS" 16 60
	sed 's/^/# /' "$HF"
	POLB=$(nm_policy "$HPID" "$HADDR")
	echo "$D1,$PN" > "$g/cpuset.mems" 2>/dev/null; sleep 2		# drop D0
	SPA=$(usernuma_sorted "$D1,$PN"); EXPA=$(foldpos "$POS" $SPA)
	POLA=$(nm_policy "$HPID" "$HADDR"); GOTA=$(polnodes "$POLA")
	ktap_print_msg "R4 before space={$SPB} pol=$POLB (expect $EXPB); after drop D0 space={$SPA} expect $EXPA pol=$POLA"
	if nodelist_has "${GOTA:-x}" "$EXPA"; then
		ktap_test_pass "R4 relative re-folded pos $POS onto $EXPA over the new USER_NUMA space (pol=$POLA)"
	else
		ktap_test_fail "R4 relative rebind expected node $EXPA, got {$GOTA} (positions mishandled as node ids?)"
	fi
	reap "$g"
fi

# ========================================================================
# R3: PN device-owned (NOT USER_NUMA) but present in cpuset.mems.
if ! online_pn 0 || node_is_usernuma "$PN"; then
	ktap_test_skip "R3 could not online $PN as a device-owned (non-USER_NUMA) private node"
else
	g="$CG/rel_r3"; mkchild "$g"; echo "$D0,$D1,$PN" > "$g/cpuset.mems"
	ALL=$(echo "$D0 $D1 $PN" | tr ' ' '\n' | sort -n | tr '\n' ' ')
	POS=$(index_of "$PN" $ALL)			# the position PN WOULD take if counted
	SP=$(usernuma_sorted "$D0,$D1,$PN")		# device-owned PN is excluded
	EXP=$(foldpos "$POS" $SP)			# folds onto a public node instead
	start_holder "$g" bindrelative "$POS" 16 40
	sed 's/^/# /' "$HF"
	ONPN=$(nm_on_node "$HPID" "$HADDR" "$PN"); ONEXP=$(nm_on_node "$HPID" "$HADDR" "$EXP")
	POL=$(nm_policy "$HPID" "$HADDR")
	ktap_print_msg "R3 usernuma space={$SP} (PN $PN device-owned, excluded) pos=$POS -> expect $EXP ; pol=$POL onPN=$ONPN onExp=$ONEXP"
	if [ "${ONPN:-0}" -eq 0 ] 2>/dev/null && [ "${ONEXP:-0}" -gt 0 ] 2>/dev/null; then
		ktap_test_pass "R3 relative fold skipped device-owned private $PN, landed on public $EXP"
	else
		ktap_test_fail "R3 relative fold touched device-owned private $PN (onPN=$ONPN onExp=$ONEXP pol=$POL)"
	fi
	reap "$g"
fi

rm -f "$HF"
pn_reset
ktap_finished
