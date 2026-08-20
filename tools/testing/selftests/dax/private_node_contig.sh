#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Contiguous allocation on a private node, both entry points.
#
# The range variant is target-given: the caller already holds the PFNs, so
# there is no zonelist to consult and nothing to gate the allocation itself.
# What is gated is displacing the folios in the way, which is reclaim-class
# work.
#
#   1. A free range on a private node is allocated, and the pages it hands
#      back are on that node rather than somewhere in the fallback list.
#   2. With the node's folios resident and FEAT_RECLAIM set, the same
#      allocation succeeds by migrating them out of the way.
#   3. Without FEAT_RECLAIM it is refused, leaving the folios where they are.
#
# The search variant picks the range itself, so which nodes it may see is the
# whole question.
#
#   4. A plain search naming a private node does not land on it: the node is
#      absent from its own fallback list, so the search never sees it.
#   5. A search that asks for the private node does land on it.
#
# Needs the dax_test provider with a carved range (PN_DAX_TEST_RANGE_START /
# _SIZE / _NODE); skips otherwise, since the PFNs come from that range.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CONTIG=$DAX_TEST_DEBUGFS/contig

# A chunk big enough to be interesting, small enough to sit well inside the
# aligned-down portion of the carved range that actually came online.
CHUNK_PAGES=512			# 2MB

# For the cells that need resident folios in the way: fill the node almost
# full so any window has some, and ask for a window big enough to be sure of
# that but small enough that migrating its occupants out can actually succeed.
BIG_PAGES=16384			# 64MB
FILL_PCT=90

contig_set()  { echo "range $1 $2" > "$CONTIG" 2>/dev/null; }
contig_drop() { echo free > "$CONTIG" 2>/dev/null; }
contig_field() { sed -n "s/.*$1=\(-\?[0-9]*\).*/\1/p" "$CONTIG" 2>/dev/null; }

# First PFN of a node, from its memory-block symlinks.  Block size is hex.
node_first_pfn() {
	local nid=$1 blk bs
	bs=$(cat /sys/devices/system/memory/block_size_bytes 2>/dev/null) || return 1
	blk=$(find "$NODE_BASE/node$nid" -maxdepth 1 -name 'memory[0-9]*' -printf '%f\n' 2>/dev/null |
	      sed 's/^memory//' | sort -n | head -1)
	[ -n "$blk" ] || return 1
	echo $(( blk * 0x$bs / 4096 ))
}

# A private node that did NOT opt into reclaim, if the topology has one.
find_noreclaim_node() {
	local n nid
	for n in "$NODE_BASE"/node[0-9]*; do
		nid=${n##*node}
		[ -r "$n/meminfo" ] || continue
		pn_node_is_private "$nid" || continue
		pn_node_has_feature "$nid" "$(pn__feat_mask reclaim)" && continue
		echo "$nid"; return 0
	done
	return 1
}

# Free pages (kB) on a node, from its meminfo.
node_free_kb() {
	sed -n 's/^Node '"$1"' MemFree: *\([0-9]*\) kB/\1/p' \
		"$NODE_BASE/node$1/meminfo"
}

# Fill most of the node with bound anonymous memory, held in the background.
# Echoes the pid so the caller can reap it.
fill_node() {
	local nid=$1 mb=$2 out=$3
	pn_anon_bind "$nid" || return 1
	( "$TOOL" daxmaphold "$PN_ANON" "$mb" "$nid" 30 ) >"$out" 2>&1 &
	echo $!
}

pn_begin
pn_require_root
pn_require_tool
pn_provision

[ -n "${PN_DAX_TEST_RANGE_START:-}" ] ||
	{ ktap_skip_all "no carved range (set PN_DAX_TEST_RANGE_START/_SIZE/_NODE)"; exit "$KSFT_SKIP"; }
[ -w "$CONTIG" ] ||
	{ ktap_skip_all "dax_test has no contig knob (need CONFIG_CONTIG_ALLOC)"; exit "$KSFT_SKIP"; }

ktap_set_plan 5

# The carved-range cells need the provider's own node, which an earlier test
# in the same guest may have left unprovisioned.  Re-establish it rather than
# inheriting whatever is there; pn_provision only guarantees *some* private
# node, not this one.
PN=${PN_DAX_TEST_NODE}
if ! pn_node_is_private "$PN"; then
	# The module may still be loaded with its device unbound from kmem, in
	# which case modprobe is a no-op and the node simply has no memory.
	pn_provider_load
	pn__bind_one 2>/dev/null
	sleep 1
fi

# ---------------------------------------------------------------------------
# 1. free range -> allocated, and it stays on the private node
# ---------------------------------------------------------------------------
if ! pn_node_is_private "$PN"; then
	for i in 1 2 3 4 5; do
		ktap_test_skip "$i node $PN is not private"
	done
	ktap_finished
fi

contig_drop
contig_set 0 "$CHUNK_PAGES"
rc=$(contig_field rc); nid=$(contig_field nid)
ktap_print_msg "free-range alloc: rc=$rc nid=$nid (want rc=0 nid=$PN)"
if [ "$rc" = 0 ] && [ "$nid" = "$PN" ]; then
	ktap_test_pass "1 contiguous range allocated on private node $PN"
else
	ktap_test_fail "1 free-range alloc rc=$rc nid=$nid (wanted rc=0 nid=$PN)"
fi
contig_drop

# ---------------------------------------------------------------------------
# 2. resident folios + FEAT_RECLAIM -> migrated out of the way, succeeds
# ---------------------------------------------------------------------------
if ! pn_node_has_feature "$PN" "$(pn__feat_mask reclaim)"; then
	ktap_test_skip "2 node $PN has no FEAT_RECLAIM"
else
	TOTAL_MB=$(( PN_DAX_TEST_RANGE_SIZE / 1024 / 1024 ))
	FILL_MB=$(( TOTAL_MB * FILL_PCT / 100 ))
	HF=$(mktemp); PID=$(fill_node "$PN" "$FILL_MB" "$HF")
	sleep 2
	free_kb=$(node_free_kb "$PN")
	contig_set 0 "$BIG_PAGES"
	rc=$(contig_field rc); nid=$(contig_field nid)
	ktap_print_msg "with residents (free=${free_kb}kB): rc=$rc nid=$nid"
	# -EPERM here would mean the gate fired on a node that has the feature.
	if [ "$rc" = -1 ]; then
		ktap_test_fail "2 gate refused (-EPERM) despite FEAT_RECLAIM"
	elif [ "$rc" = 0 ] && [ "$nid" = "$PN" ]; then
		ktap_test_pass "2 alloc succeeded over resident folios with FEAT_RECLAIM"
	else
		ktap_test_fail "2 alloc rc=$rc nid=$nid with FEAT_RECLAIM (wanted rc=0)"
	fi
	contig_drop
	kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; rm -f "$HF"
fi

# ---------------------------------------------------------------------------
# 3. resident folios, no FEAT_RECLAIM -> refused with -EPERM
# ---------------------------------------------------------------------------
# Use a node that came up without reclaim rather than re-provisioning this
# one: a node keeps the mask it was claimed with while it holds memory, and
# kmem usually cannot take that memory back, so a reload would silently leave
# the old mask in place.
NR_NODE=$(find_noreclaim_node)
NR_PFN=$([ -n "$NR_NODE" ] && node_first_pfn "$NR_NODE")

if [ -z "$NR_NODE" ] || [ -z "$NR_PFN" ]; then
	ktap_test_skip "3 no private node without FEAT_RECLAIM (boot one with private_node=<nid>,0x80)"
else
	NR_TOTAL_KB=$(sed -n 's/^Node '"$NR_NODE"' MemTotal: *\([0-9]*\) kB/\1/p' \
		      "$NODE_BASE/node$NR_NODE/meminfo")
	FILL_MB=$(( NR_TOTAL_KB / 1024 * FILL_PCT / 100 ))
	HF=$(mktemp); PID=$(fill_node "$NR_NODE" "$FILL_MB" "$HF")
	sleep 2
	free_kb=$(node_free_kb "$NR_NODE")
	echo "pfn $NR_PFN $BIG_PAGES" > "$CONTIG" 2>/dev/null
	rc=$(contig_field rc)
	ktap_print_msg "node $NR_NODE no FEAT_RECLAIM, pfn=$NR_PFN free=${free_kb}kB: rc=$rc (want -1/EPERM)"
	# -EPERM is itself proof that folios were isolated and then refused:
	# the gate is only reached once something needs displacing.  Only
	# consider the fill level when deciding what a *success* means.
	if [ "$rc" = -1 ]; then
		ktap_test_pass "3 alloc refused with -EPERM without FEAT_RECLAIM"
	elif [ "${free_kb:-0}" -gt $(( NR_TOTAL_KB / 2 )) ]; then
		ktap_test_skip "3 node $NR_NODE did not fill; nothing resident to displace"
	elif [ "$rc" = 0 ]; then
		ktap_test_fail "3 alloc succeeded without FEAT_RECLAIM (displaced folios anyway)"
	else
		ktap_test_fail "3 alloc failed rc=$rc, wanted -1/EPERM (gate did not fire)"
	fi
	contig_drop
	kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; rm -f "$HF"
fi

# Restore the provisioning the rest of the suite expects.
modprobe -r dax_test 2>/dev/null
pn_provider_load
sleep 1

# ---------------------------------------------------------------------------
# 4. searched allocation naming a private node WITHOUT asking for one
# ---------------------------------------------------------------------------
# alloc_contig_pages() consults node_zonelist(), and a private node is absent
# from its own fallback list, so the search runs over public memory.  It must
# not come back claiming to have satisfied the request on the private node.
if ! pn_node_is_private "$PN"; then
	ktap_test_skip "4 node $PN is not private"
else
	contig_drop
	echo "public $CHUNK_PAGES" > "$CONTIG" 2>/dev/null
	rc=$(contig_field rc); nid=$(contig_field nid)
	ktap_print_msg "unasked search on private $PN: rc=$rc nid=$nid"
	if [ "$rc" = 0 ] && [ "$nid" = "$PN" ]; then
		ktap_test_fail "4 plain search landed on private node $PN unasked"
	else
		ktap_test_pass "4 plain search did not land on private node $PN (rc=$rc nid=$nid)"
	fi
	contig_drop
fi

# ---------------------------------------------------------------------------
# 5. searched allocation that DOES ask for the private node
# ---------------------------------------------------------------------------
if ! pn_node_is_private "$PN"; then
	ktap_test_skip "5 node $PN is not private"
else
	contig_drop
	echo "pages $CHUNK_PAGES" > "$CONTIG" 2>/dev/null
	rc=$(contig_field rc); nid=$(contig_field nid)
	ktap_print_msg "private search on $PN: rc=$rc nid=$nid (want rc=0 nid=$PN)"
	if [ "$rc" = 0 ] && [ "$nid" = "$PN" ]; then
		ktap_test_pass "5 private search allocated on private node $PN"
	else
		ktap_test_fail "5 private search rc=$rc nid=$nid (wanted rc=0 nid=$PN)"
	fi
	contig_drop
fi

ktap_finished
