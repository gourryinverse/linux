#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM's reserved pages are balloon pages, which means they are MOVABLE.  That
# is the whole reason for going through mm/balloon.c rather than holding a
# private list, and it has two consequences worth asserting separately:
#
#   1. compaction can shuffle a reservation within the node.  Without this a
#      convergence leaves the node permanently fragmented -- the reservations
#      are immovable obstacles and nothing can ever move them, which matters
#      because CRAM carries PMD-order folios.
#   2. offline can evacuate a reservation off the node.  Without this a memory
#      block holding reserved pages simply cannot be offlined.
#
# Case 2 is not reachable through the normal unplug path: cram_unregister()
# deflates the balloon before it offlines anything.  So the test offlines a
# single memory block directly, which is the path a real hot-unplug of part of
# the node takes.
#
# Both cases run through cram_balloon_migratepage(), which is otherwise dead
# code -- nothing else in the suite exercises it.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

DAX_BASE=/sys/bus/dax/devices
CRAM_DBG=/sys/kernel/debug/cram

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e "$CRAM_DBG/nodes" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
grep -q '^balloon_migrate ' /proc/vmstat ||
	{ ktap_skip_all "no balloon_migrate vmstat (CONFIG_BALLOON_MIGRATION=y?)";
	  exit "$KSFT_SKIP"; }

cram_provision() {
	local d nid drv r

	pn_modprobe nd_e820 dax_pmem device_dax nd_pmem
	modprobe -q kmem 2>/dev/null
	[ -d /sys/bus/dax/drivers/kmem ] ||
		{ ktap_skip_all "kmem driver unavailable"; exit "$KSFT_SKIP"; }
	if command -v ndctl >/dev/null 2>&1; then
		for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
			ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f \
				>/dev/null 2>&1
		done
	fi
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node"); [ "$nid" -ge 0 ] 2>/dev/null || continue
		node_in_mask "$nid" has_memory && continue
		drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
		[ "$drv" = device_dax ] &&
			basename "$d" > /sys/bus/dax/drivers/device_dax/unbind 2>/dev/null
		[ "$drv" = kmem ] ||
			basename "$d" > /sys/bus/dax/drivers/kmem/new_id 2>/dev/null
		sleep 1
		D=$d; PN=$nid
		[ -e "$D/cram" ] ||
			{ ktap_skip_all "$(basename "$d") has no cram knob"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

balloon()   { awk -v n="$PN" '$1==n {print $5}' "$CRAM_DBG/nodes"; }
present()   { awk -v n="$PN" '$1==n {print $4}' "$CRAM_DBG/nodes"; }
vmev()      { awk -v k="$1" '$1==k {print $2}' /proc/vmstat; }
ctl()       { echo "$*" > "$CRAM_DBG/control"; }

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
trap 'ctl "deflate $PN 4294967295" 2>/dev/null;
      echo unplugged > "$D/state" 2>/dev/null' EXIT

ktap_set_plan 3
dmesg -C 2>/dev/null

# Reserve about half the node.  Half rather than all: compaction needs free
# pages to migrate into, and the offline case needs the rest of the node to
# have somewhere to put what it evacuates.
NR=$(( $(present) / 2 ))
ctl "inflate $PN $NR"
BAL0=$(balloon)
ktap_print_msg "cram node $PN present=$(present) balloon=$BAL0"
[ "${BAL0:-0}" -ge 4096 ] ||
	{ ktap_skip_all "balloon only reached ${BAL0:-0} pages"; exit "$KSFT_SKIP"; }

# ---------------------------------------------------------------------------
# 1. a reservation can be migrated WITHIN the node
#
# Driven with alloc_contig_range() rather than compaction.  Compaction is a
# heuristic: it walks a migrate scanner up and a free scanner down, and on a
# node that is half reservations at one end and half free at the other it
# concludes there is nothing to gain and stops -- measured, it scanned 32 pages
# and moved none.  alloc_contig_range() has no such choice to make: it must
# empty the range it was given.  For a private node the migration target is the
# same node, so this is the same-node arm of cram_balloon_migratepage() -- the
# one that re-trims the replacement and leaves the balloon count alone.
# ---------------------------------------------------------------------------
modprobe -q dax_test 2>/dev/null
CONTIG=/sys/kernel/debug/dax_test/contig
BLKSZ=$(( 0x$(cat /sys/devices/system/memory/block_size_bytes) / 4096 ))

if [ ! -e "$CONTIG" ]; then
	ktap_test_skip "no dax_test contig verb to force migration with"
else
	MIG0=$(vmev balloon_migrate); BALa=$(balloon); hit=0
	for m in /sys/devices/system/node/node"$PN"/memory*; do
		[ -e "$m/state" ] || continue
		[ "$(cat "$m/state")" = online ] || continue
		blkno=$(basename "$m" | tr -dc '0-9')
		# a pageblock-sized window a little way into the block
		start=$(( blkno * BLKSZ + 1024 ))
		echo "pfn $start 512" > "$CONTIG" 2>/dev/null
		ktap_print_msg "contig pfn $start: $(cat "$CONTIG" 2>&1)"
		echo free > "$CONTIG" 2>/dev/null
		[ "$(vmev balloon_migrate)" -gt "$MIG0" ] && { hit=1; break; }
	done
	MIG1=$(vmev balloon_migrate); BALb=$(balloon)

	if [ "$hit" = 1 ] && [ "$BALb" = "$BALa" ]; then
		ktap_test_pass "alloc_contig migrated $((MIG1 - MIG0)) reserved pages within node $PN, balloon unchanged at $BALb"
	elif [ "$hit" != 1 ]; then
		ktap_test_fail "no reserved page was migrated within the node (balloon_migrate $MIG0 -> $MIG1, balloon $BALa)"
	else
		ktap_test_fail "same-node migration changed the balloon: $BALa -> $BALb"
	fi
fi

# ---------------------------------------------------------------------------
# 2. offline evacuates reserved pages off the node
#
# Reserved pages used to be plain refcounted pages, so a block holding them
# could not be offlined at all.  Off-node is where the reservation ENDS -- it
# exists to shrink this node -- so the balloon must shrink by what left.
# ---------------------------------------------------------------------------
# Which block holds reservations is not predictable, so walk them until one
# does.  A block with none offlines trivially and proves nothing.
moved=0 tried=0 refused=0 blk=
for m in /sys/devices/system/node/node"$PN"/memory*; do
	[ -e "$m/state" ] || continue
	[ "$(cat "$m/state")" = online ] || continue
	BAL2=$(balloon); DEF0=$(vmev balloon_deflate)
	tried=$((tried + 1))
	if echo offline > "$m/state" 2>/dev/null; then
		sleep 1
		BAL3=$(balloon)
		moved=$((BAL2 - BAL3))
		blk=$(basename "$m")
		echo online > "$m/state" 2>/dev/null
		sleep 1
		[ "$moved" -gt 0 ] && break
	else
		refused=$((refused + 1))
	fi
done

if [ "$moved" -gt 0 ]; then
	ktap_test_pass "offline of $blk evacuated $moved reserved pages off the node (deflate +$(( $(vmev balloon_deflate) - DEF0 )))"
elif [ "$refused" -gt 0 ]; then
	ktap_test_fail "$refused of $tried blocks refused to offline while holding reserved pages -- reservations are not movable"
else
	ktap_test_fail "all $tried blocks offlined but no reserved page ever moved (balloon $(balloon))"
fi

# ---------------------------------------------------------------------------
# 3. no splat
# ---------------------------------------------------------------------------
splat=$(dmesg 2>/dev/null | grep -acE "KASAN|BUG:|Oops|WARNING:")
if [ "$splat" = 0 ]; then
	ktap_test_pass "no KASAN/BUG/UAF/WARN across reserved-page migration"
else
	ktap_test_fail "kernel splat during reserved-page migration (count=$splat)"
	dmesg | grep -aE "KASAN|BUG:|Oops|WARNING:" | tail -5
fi

ktap_finished
