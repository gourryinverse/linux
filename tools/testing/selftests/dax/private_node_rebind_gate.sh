#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# A cpuset rebind cannot hand a task a node it could never have asked for.
#
# cpuset.mems belongs to the admin and a rebind adheres to whatever they write,
# however little sense it makes.  What it must not do is turn the positional
# remap into a side door: MPOL_BIND's default remap moves a policy to whatever
# node occupies the same relative slot in the new mask, and it has no idea what
# is in that slot.  With two private nodes of different classes, one that opted
# into userspace placement and one that did not, stock semantics would quietly
# rewrite a legal binding into an illegal one.
#
# mpol_rebind_nodemask() partitions the mask so that cannot happen: the
# positional remap runs over the USER_NUMA subsets only, so a node without the
# opt-in is never a remap destination, and such a node already in a policy goes
# through a separate "keep" set -- carried verbatim while it stays in the
# cpuset, dropped when it leaves, never moved.
#
#   P = private, opted into USER_NUMA      Q = private, NOT opted in
#
#   R1 bind({P}), then the admin swaps P out of cpuset.mems for Q.  Q takes the
#      slot P vacated, so a stock positional remap lands the policy on Q.  It
#      must collapse to the public nodes instead.  This is the rebind-time twin
#      of G8 in private_node_cpuset_governed.sh, which is the deny at set time.
#   R2 a driver-owned bind on Q is KEPT when the cpuset GROWS to include P: Q is
#      not positionally moved onto P, and P is not added to a policy that never
#      asked for it.
#   R3 a driver-owned bind on Q whose node the admin removes from cpuset.mems is
#      left with an empty nodemask -- the reset to public memory is deliberately
#      conditional on the policy having had a USER_NUMA node to remap in the
#      first place.  An empty bind is one policy_nodemask() declines to apply,
#      dropping ALLOC_ZONELIST_PRIVATE with it, so the mapping degrades to
#      public memory: fresh faults complete and the task survives.  G9 covers
#      the same scrub driven by a node offline; this is the cpuset.mems trigger.
#
#      Note what R3 records rather than asserts: pages ALREADY resident on Q
#      stay there.  A mems edit scrubs the policy, it does not migrate, so the
#      mapping ends up split between a node the cpuset no longer grants and the
#      public nodes.  An offline migrates them (G9) because the node is leaving
#      N_MEMORY; an admin narrowing cpuset.mems does not.  A driver that would
#      rather fail loudly than be served public memory has nowhere to say so
#      today -- if that changes, this is the cell that changes with it.
#
# Needs a public node, a USER_NUMA private node, a non-USER_NUMA private node
# and cgroup2.  regress.sh's private_node=1,0x186 and private_node=3,0x102 are
# the reference pair.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_root
pn_require_tool
pn_provision

CG=$(pn_cgroup2) || { ktap_skip_all "cgroup2 cpuset unavailable"; pn_reset; exit "$KSFT_SKIP"; }
P=$(pn_find_node usernuma)
Q=$(pn_find_node plain)
D0=$(pn_dram_list); set -- $D0; D0=$1
CPUS=$(cat "$CG/cpuset.cpus.effective")

if [ -z "$P" ] || [ -z "$Q" ] || [ -z "$D0" ]; then
	ktap_skip_all "need public + opted-in + opted-out private nodes\
(have D0=${D0:-none} P=${P:-none} Q=${Q:-none})"
	pn_reset; exit "$KSFT_SKIP"
fi
ktap_print_msg "public D0=$D0; USER_NUMA private P=$P; opted-out private Q=$Q; cpus=$CPUS"
ktap_set_plan 3

HF=/tmp/pn_rbg.$$
mkchild() { mkdir -p "$1" 2>/dev/null; echo "$CPUS" > "$1/cpuset.cpus" 2>/dev/null
	    echo "$2" > "$1/cpuset.mems" 2>/dev/null; }
reap() { echo $$ > "$CG/cgroup.procs" 2>/dev/null; kill -9 "${J:-0}" 2>/dev/null
	 wait "${J:-0}" 2>/dev/null; rmdir "$1" 2>/dev/null; }
# hold CG ARGS... -- run the tool in CG, wait for it to announce pid=, set HPID/HADDR
hold() {
	local g=$1; shift
	: > "$HF"
	( echo $BASHPID > "$g/cgroup.procs"; exec "$TOOL" "$@" ) >"$HF" 2>&1 &
	J=$!
	for _ in $(seq 1 40); do grep -qE "pid=" "$HF" && break; sleep 0.2; done
	HPID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$HF" | head -1)
	HADDR=$(sed -n 's/.*addr=\(0x[0-9a-f]*\).*/\1/p' "$HF" | head -1)
}
# nodelist of the policy on HPID's VMA at HADDR ("" when there is no bind)
polnl() { local p; p=$(nm_policy "$1" "$2"); case "$p" in *:*) echo "${p#*:}";; *) echo "";; esac; }

# --- R1: a USER_NUMA bind is never remapped onto an opted-out node ----------
g="$CG/rbg1"; mkchild "$g" "$D0,$P"
hold "$g" mbindhold bind "$P" 16 40
pol0=$(nm_policy "$HPID" "$HADDR")
if [ -z "$HPID" ] || ! nodelist_has "$(polnl "$HPID" "$HADDR")" "$P"; then
	sed 's/^/# /' "$HF"
	ktap_test_skip "R1 could not establish bind({$P}) (pol='$pol0')"
else
	echo "$D0,$Q" > "$g/cpuset.mems" 2>/dev/null		# P out, Q in its slot
	sleep 2
	nl1=$(polnl "$HPID" "$HADDR"); pol1=$(nm_policy "$HPID" "$HADDR")
	alive=$(kill -0 "$HPID" 2>/dev/null && echo 1 || echo 0)
	ktap_print_msg "R1 pol '$pol0' -> '$pol1' alive=$alive"
	if nodelist_has "${nl1:-x}" "$Q"; then
		ktap_test_fail "R1 rebind put the policy on opted-out $Q (pol='$pol1')"
	elif [ -n "$nl1" ] && nodelist_has "$nl1" "$D0" && [ "$alive" = 1 ]; then
		ktap_test_pass "R1 bind({$P}) collapsed to public {$nl1}, not onto opted-out $Q"
	else
		ktap_test_fail "R1 expected a public bind after the swap (pol='$pol1' alive=$alive)"
	fi
fi
reap "$g"

# --- R2: an opted-out node is kept verbatim, and P is not added -------------
g="$CG/rbg2"; mkchild "$g" "$D0,$Q"
if ! pn_anon_bind "$Q" 2>/dev/null; then
	ktap_test_skip "R2 needs the test provider's anon knob to place on $Q"
else
	hold "$g" daxmaphold "$PN_ANON" 16 "$Q" 40
	pol0=$(nm_policy "$HPID" "$HADDR")
	if [ -z "$HPID" ] || ! nodelist_has "$(polnl "$HPID" "$HADDR")" "$Q"; then
		sed 's/^/# /' "$HF"
		ktap_test_skip "R2 driver-owned bind on $Q not established (pol='$pol0')"
	else
		echo "$D0,$Q,$P" > "$g/cpuset.mems" 2>/dev/null	# grow the set
		sleep 2
		nl1=$(polnl "$HPID" "$HADDR"); pol1=$(nm_policy "$HPID" "$HADDR")
		ktap_print_msg "R2 pol '$pol0' -> '$pol1'"
		if nodelist_has "${nl1:-x}" "$P"; then
			ktap_test_fail "R2 $P added to a bind on $Q (pol='$pol1')"
		elif nodelist_has "${nl1:-x}" "$Q"; then
			ktap_test_pass "R2 opted-out $Q kept verbatim across the grow, $P not added"
		else
			ktap_test_fail "R2 lost the bind to $Q on a grow (pol='$pol1')"
		fi
	fi
fi
reap "$g"

# --- R3: removing the bound node degrades to public, it does not wedge ------
g="$CG/rbg3"; mkchild "$g" "$D0,$Q"
if ! pn_anon_bind "$Q" 2>/dev/null; then
	ktap_test_skip "R3 needs the test provider's anon knob to place on $Q"
else
	: > "$HF"
	( echo $BASHPID > "$g/cgroup.procs"
	  exec "$TOOL" daxmaprefault "$PN_ANON" 16 "$Q" 6 ) >"$HF" 2>&1 &
	J=$!
	for _ in $(seq 1 40); do grep -q phase1 "$HF" && break; sleep 0.2; done
	HPID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$HF" | head -1)
	HADDR=$(sed -n 's/.*addr=\(0x[0-9a-f]*\).*/\1/p' "$HF" | head -1)
	pol0=$(nm_policy "$HPID" "$HADDR")
	# The tool reads its own numa_maps at each phase; use that rather than a
	# read from here, which races the munmap after phase 2.  Requiring phase 1
	# to have landed on Q keeps a pass from meaning "never got there at all".
	onQ0=$(sed -n "s/.*phase1 .*on_node$Q=\([0-9]*\).*/\1/p" "$HF" | head -1)
	if ! grep -q phase1 "$HF" || [ "${onQ0:-0}" -le 0 ] 2>/dev/null; then
		sed 's/^/# /' "$HF"
		ktap_test_skip "R3 mapping not resident on $Q (on$Q=${onQ0:-0} pol='$pol0')"
	else
		echo "$D0" > "$g/cpuset.mems" 2>/dev/null	# admin drops Q
		for _ in $(seq 1 60); do grep -q phase2 "$HF" && break; sleep 0.5; done
		nl1=$(polnl "$HPID" "$HADDR"); pol1=$(nm_policy "$HPID" "$HADDR")
		tot1=$(sed -n 's/.*phase2 total=\([0-9]*\).*/\1/p' "$HF" | head -1)
		onQ1=$(sed -n "s/.*phase2 .*on_node$Q=\([0-9]*\).*/\1/p" "$HF" | head -1)
		sed 's/^/# /' "$HF"
		ktap_print_msg "R3 pol '$pol0' -> '$pol1' ; on$Q $onQ0 -> ${onQ1:-?}"
		ktap_print_msg "R3 of ${tot1:-?} total -- a mems edit scrubs, it does not migrate"
		if ! grep -q phase2 "$HF"; then
			ktap_test_fail "R3 fresh faults never completed (pol='$pol1')"
		elif nodelist_has "${nl1:-x}" "$Q"; then
			ktap_test_fail "R3 bind to $Q survived its removal (pol='$pol1')"
		else
			ktap_test_pass "R3 bind to $Q scrubbed, fresh faults done on public"
		fi
	fi
fi
reap "$g"

rm -f "$HF"
pn_reset
ktap_finished
