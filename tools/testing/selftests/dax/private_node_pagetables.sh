#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# apply_policy_zone() for private nodes: a process-wide MPOL_BIND to a private
# node binds the task's UNMOVABLE allocations (page tables) too, but only when
# the node has a non-movable zone.
#
# This exercises policy_private_has_kernel_zone().
#
#   1. kernel-zoned (online_kernel) private node: page tables land ON the node
#      (node PageTables grows) - the bind applies at the page-table zone.
#
#   2. movable-only (online_movable) private node: page tables spill OFF the
#      node (node PageTables stays flat) - the policy collapses to ZONE_MOVABLE,
#      so the unmovable allocation falls back rather than livelocking.
#
#   3. a kernel-zoned private node holding bound page tables cannot be unplugged
#      while the task lives, but CAN be unplugged if the task is reaped.
#
#      This is a property an ordinary ZONE_NORMAL node (fed unmovable allocs
#      from everywhere) cannot offer, since nothing reaches a private node
#      except an explicit bind.
#
#      That means we can safely assert that this node will ALWAYS become able
#      to be hot-unplugged once the only task holding an mbind is reaped.
#
# Needs a kmem-bindable dax device on a memoryless node. SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pt_kb() { awk '$0 ~ /^Node [0-9]+ PageTables:/{print $4}' "$NODE_BASE/node$PN/meminfo" 2>/dev/null; }
nt_mb() { awk '/MemTotal:/{print int($4/1024)}' "$NODE_BASE/node$PN/meminfo" 2>/dev/null; }

pn_begin
pn_require_tool
pn_provision
pn_reset
ktap_set_plan 3

# Fault enough to make the page-table footprint visible (~1 PTE page / 2MB).
fault_mb() { local n; n=$(nt_mb); echo $(( n * 60 / 100 )); }
GROW=256		# kB: a clear on-node page-table delta
FLAT=128		# kB: tolerance for "spilled off node"

# pt_settle -- return the node to a clean unplugged baseline between subtests.
# The leading sleep lets a just-reaped task's pages drain so the unplug succeeds.
pt_settle() { sleep 1; pn_reset; sleep 1; }

# ---------------------------------------------------------------------------
# 1. movable-only private node: bound page tables spill OFF the node.
#    (Done first: a movable node always drains cleanly for the next online.)
# ---------------------------------------------------------------------------
pn_set user_numa 1
pn_set hotunplug 1		# CAP_HOTUNPLUG gates offline; needed to drain after
pn_hotplug online_movable
mb=$(fault_mb)
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_test_skip "could not online node $PN as a movable private node (state=$(pn_state))"
elif [ "${mb:-0}" -lt 64 ]; then
	ktap_test_skip "node $PN too small (${mb}MB) for the spill check"
else
	pt0=$(pt_kb)
	"$TOOL" bindfault "$PN" "$mb" 25 >/dev/null 2>&1 & bf=$!
	sleep 6
	pt1=$(pt_kb)
	kill "$bf" 2>/dev/null; wait "$bf" 2>/dev/null
	d=$(( ${pt1:-0} - ${pt0:-0} ))
	ktap_print_msg "movable-only: PageTables ${pt0}kB -> ${pt1}kB (+${d}kB) for ${mb}MB bind"
	if [ "$d" -le "$FLAT" ]; then
		ktap_test_pass "movable-only private node $PN spilled page tables off-node (+${d}kB, no wedge)"
	else
		ktap_test_fail "page tables landed on movable-only private node $PN (+${d}kB)"
	fi
fi
pt_settle

# ---------------------------------------------------------------------------
# 2. kernel-zoned private node: bound page tables land ON the node.
# ---------------------------------------------------------------------------
pn_set user_numa 1
pn_set hotunplug 1
pn_hotplug online_kernel
mb=$(fault_mb)
if [ "$(pn_state)" != online_kernel ] || ! pn_is_private; then
	ktap_test_skip "could not online node $PN as a kernel-zoned private node (state=$(pn_state))"
elif [ "${mb:-0}" -lt 64 ]; then
	ktap_test_skip "node $PN too small (${mb}MB) for a page-table delta"
else
	pt0=$(pt_kb)
	"$TOOL" bindfault "$PN" "$mb" 25 >/dev/null 2>&1 & bf=$!
	sleep 6
	pt1=$(pt_kb)
	kill "$bf" 2>/dev/null; wait "$bf" 2>/dev/null
	d=$(( ${pt1:-0} - ${pt0:-0} ))
	ktap_print_msg "kernel-zoned: PageTables ${pt0}kB -> ${pt1}kB (+${d}kB) for ${mb}MB bind"
	if [ "$d" -ge "$GROW" ]; then
		ktap_test_pass "kernel-zoned private node $PN held bound page tables (+${d}kB)"
	else
		ktap_test_fail "page tables did not land on kernel-zoned private node $PN (+${d}kB)"
	fi
fi
pt_settle

# ---------------------------------------------------------------------------
# 3. kernel-zoned node drains + unplugs once the bound task is reaped.
# ---------------------------------------------------------------------------
pn_set user_numa 1
pn_set hotunplug 1
pn_hotplug online_kernel
mb=$(fault_mb)
if [ "$(pn_state)" != online_kernel ] || ! pn_is_private; then
	ktap_test_skip "could not online node $PN as a kernel-zoned private node for the unplug check"
elif [ "${mb:-0}" -lt 64 ]; then
	ktap_test_skip "node $PN too small (${mb}MB) for the unplug check"
else
	"$TOOL" bindfault "$PN" "$mb" 90 >/dev/null 2>&1 & bf=$!
	sleep 6
	# While the task holds bound (unmovable) page tables, unplug must fail.
	timeout 20 sh -c "echo unplugged > '$D/state'" 2>/dev/null
	held=$(pn_state)
	# Reap the task: its page tables free, the node drains.
	kill "$bf" 2>/dev/null; wait "$bf" 2>/dev/null
	sleep 1
	pn_hotplug unplugged
	after=$(pn_state)
	ktap_print_msg "unplug while held -> state=$held ; after reap -> state=$after"
	if [ "$after" = unplugged ] && [ "$held" != unplugged ]; then
		ktap_test_pass "kernel-zoned private node $PN drained and unplugged after task reaped (was pinned while held)"
	elif [ "$after" = unplugged ]; then
		ktap_test_pass "kernel-zoned private node $PN unplugged after task reaped (held-state inconclusive: $held)"
	else
		ktap_test_fail "node $PN did not unplug after the bound task was reaped (state=$after)"
	fi
fi
pn_reset

ktap_finished
