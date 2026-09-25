#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM's reserved pages are balloon pages, which means they are MOVABLE.  That
# is the whole reason for going through mm/balloon.c rather than holding a
# private list: hotplug can evacuate reservations off the node.  Without this a
# memory block holding reserved pages simply cannot be offlined.
#
# This is not reachable through the normal unplug path: cram_unregister()
# deflates the balloon before it offlines anything.  So the test offlines a
# node's memory blocks directly, which exercises cram_balloon_migratepage().

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

DAX_BASE=/sys/bus/dax/devices

ktap_print_header
pn_require_root

grep -q '^balloon_migrate ' /proc/vmstat ||
	{ ktap_skip_all "no balloon_migrate vmstat (CONFIG_BALLOON_MIGRATION=y?)";
	  exit "$KSFT_SKIP"; }

cram_provision() {
	local d nid r

	pn_modprobe nd_e820 dax_pmem device_dax nd_pmem
	modprobe -q cramdax 2>/dev/null
	[ -d /sys/bus/dax/drivers/cramdax ] ||
		{ ktap_skip_all "cramdax driver unavailable"; exit "$KSFT_SKIP"; }
	if command -v ndctl >/dev/null 2>&1; then
		for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
			ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f \
				>/dev/null 2>&1
		done
	fi
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node"); [ "$nid" -ge 0 ] 2>/dev/null || continue
		pn_bind_cramdax "$d" || continue
		sleep 1
		D=$d; PN=$nid
		[ -e "$D/state" ] ||
			{ ktap_skip_all "$(basename "$d") has no cram knob"; exit "$KSFT_SKIP"; }
		echo offline > "$D/state" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

balloon()   { pn_node_vmstat "$PN" nr_balloon_pages; }
present()   { pn_node_present_pages "$PN"; }
vmev()      { awk -v k="$1" '$1==k {print $2}' /proc/vmstat; }
set_balloon() { echo "$1" > "$D/balloon_target"; }

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
trap 'set_balloon 0 2>/dev/null;
      echo offline > "$D/state" 2>/dev/null' EXIT

ktap_set_plan 2
dmesg -C 2>/dev/null

# Reserve about half the node.  Half rather than all: compaction needs free
# pages to migrate into, and the offline case needs the rest of the node to
# have somewhere to put what it evacuates.
NR=$(( $(present) / 2 ))
set_balloon "$NR"
for ((i = 0; i < 200; i++)); do
	[ "$(balloon)" -ge "$NR" ] 2>/dev/null && break
	sleep 0.1
done
BAL0=$(balloon)
ktap_print_msg "cram node $PN present=$(present) balloon=$BAL0"
[ "${BAL0:-0}" -ge 4096 ] ||
	{ ktap_skip_all "balloon only reached ${BAL0:-0} pages"; exit "$KSFT_SKIP"; }

# ---------------------------------------------------------------------------
# 1. offline evacuates reserved pages off the node
#
# Reserved pages used to be plain refcounted pages, so a block holding them
# could not be offlined at all.  Off-node is where the reservation ENDS -- it
# exists to shrink this node -- so the balloon must shrink by what left.
# ---------------------------------------------------------------------------
# Keep each block offline.  Re-onlining it immediately would give later blocks
# a same-node migration target and leave the total reservation unchanged.
offlined=""; tried=0; refused=0
DEF0=$(vmev balloon_deflate)
for m in /sys/devices/system/node/node"$PN"/memory*; do
	[ -e "$m/state" ] || continue
	[ "$(cat "$m/state")" = online ] || continue
	path=$(readlink -f "$m")
	tried=$((tried + 1))
	if echo offline > "$path/state" 2>/dev/null; then
		offlined="$offlined $path"
	else
		refused=$((refused + 1))
	fi
done
sleep 1
DEF1=$(vmev balloon_deflate)
moved=$((DEF1 - DEF0))
for m in $offlined; do
	echo online > "$m/state" 2>/dev/null
done
sleep 1

if [ "$refused" -gt 0 ]; then
	ktap_test_fail "$refused of $tried blocks refused to offline while holding reserved pages -- reservations are not movable"
elif [ "$moved" -gt 0 ]; then
	ktap_test_pass "offline evacuated $moved reserved pages off node $PN"
else
	ktap_test_fail "all $tried blocks offlined but no reserved page left node $PN"
fi

# ---------------------------------------------------------------------------
# 2. no splat
# ---------------------------------------------------------------------------
splat=$(dmesg 2>/dev/null | grep -acE "KASAN|BUG:|Oops|WARNING:")
if [ "$splat" = 0 ]; then
	ktap_test_pass "no KASAN/BUG/UAF/WARN across reserved-page migration"
else
	ktap_test_fail "kernel splat during reserved-page migration (count=$splat)"
	dmesg | grep -aE "KASAN|BUG:|Oops|WARNING:" | tail -5
fi

ktap_finished
