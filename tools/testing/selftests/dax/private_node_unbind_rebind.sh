#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Private-node owner-pointer lifetime across a driver unbind/rebind.
#
# A private node's owner (pgdat->node_private, installed by
# node_private_register()) must be dropped when the node's *last memory is
# removed*, not merely when it is offlined.  The driver unbind path
#
#   dev_dax_kmem_remove -> dax_kmem_remove_ranges -> remove_memory()
#     -> try_remove_memory() -> try_offline_node()
#
# removes the memory of an already-offline device.  If the owner is not cleared
# at that point it is left dangling at freed memory (the driver frees the
# node_private together with its per-device data), and the next bind's
# node_private_register() sees a stale, non-NULL owner and returns -EBUSY -- so
# the node fails to come back as a private node.
#
# Note offlining alone (memoryN/state) only offlines the private node; it never
# calls node_private_unregister(), so the owner legitimately persists across an
# offline/online cycle.  The owner lifetime is only exercised by add/remove,
# which is why this test drives a full unbind (remove) and rebind.
#
# Sequence: online private -> offline every block via memoryN/state -> unbind
# (removes the now-offline memory) -> rebind -> re-online private.  The node
# must return as a private node with no iomem leak.
#
# Needs a kmem-bindable dax device on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

MEM_BASE=/sys/devices/system/memory
DRV=/sys/bus/dax/drivers/kmem

# device_blocks -- print every memoryN block on the private node.  A private
# node is dedicated to its owning dax device (memoryless until the device
# onlines), so every block linked under node$PN belongs to the device.  This is
# more robust than matching /proc/iomem resource names.
device_blocks() {
	local b
	for b in "$NODE_BASE/node$PN"/memory*; do
		[ -e "$b" ] || continue
		basename "$b"
	done
}

# up_private -- (re)online PN as a private node with user_numa.
up_private() {
	pn_reset
	pn_set private 1
	pn_set user_numa 1
	pn_hotplug online_movable
}

pn_begin
pn_provision
ktap_print_msg "using $DAX on private node $PN (state was: $(pn_state))"

{ [ -w "$DRV/unbind" ] && [ -w "$DRV/bind" ]; } ||
	{ ktap_skip_all "kmem driver bind/unbind not writable"; exit "$KSFT_SKIP"; }

up_private
if [ "$(pn_state)" != online_movable ] || ! pn_is_private; then
	ktap_skip_all "could not online $DAX as a private node"
	pn_reset; exit "$KSFT_SKIP"
fi

ktap_set_plan 1

# Offline every block backing the device via the legacy memoryN/state
# interface.  This offlines the node (no online pages remain) but leaves
# the device added, so the unbind's remove_memory() succeeds cleanly.
offl_ok=1 nblk=0
for b in $(device_blocks); do
	[ -f "$MEM_BASE/$b/state" ] || continue
	nblk=$((nblk + 1))
	if [ "$(cat "$MEM_BASE/$b/state")" = online ]; then
		echo offline > "$MEM_BASE/$b/state" 2>/dev/null || offl_ok=0
	fi
done
if [ "$nblk" = 0 ] || [ "$offl_ok" = 0 ] || pn_is_private; then
	# If any block is still online the node stays private and the
	# unbind below would -EBUSY; skip rather than misreport.
	ktap_test_skip "could not offline all blocks of node $PN (nblk=$nblk offl_ok=$offl_ok still_private=$(pn_is_private && echo 1 || echo 0))"
	up_private >/dev/null 2>&1; pn_reset; ktap_finished; exit 0
fi

# Unbind the driver.  Every block is offline, so remove_memory() removes them
# and the node goes memoryless -- the point at which the owner must be dropped.
echo "$DAX" > "$DRV/unbind" 2>/dev/null
leaked=$(grep -cE " : ${DAX}\$" /proc/iomem)
priv_after_unbind=$(pn_is_private && echo 1 || echo 0)

# Rebind and bring the node back up as private.  If the owner was left dangling,
# node_private_register() here fails with -EBUSY and the re-online does not take.
echo "$DAX" > "$DRV/bind" 2>/dev/null
sleep 1
up_private; rc=$?
state=$(pn_state)
back_private=$(pn_is_private && echo 1 || echo 0)

ktap_print_msg "unbind: iomem_leaked=$leaked priv_after_unbind=$priv_after_unbind |" \
	       "rebind: online_rc=$rc state=$state back_private=$back_private"

if [ "$leaked" = 0 ] && [ "$rc" = 0 ] && [ "$state" = online_movable ] && \
   [ "$back_private" = 1 ]; then
	ktap_test_pass "private node survives unbind/rebind (owner dropped at remove; re-onlines as a private node)"
else
	ktap_test_fail "unbind/rebind lost the private owner (iomem_leaked=$leaked online_rc=$rc state=$state back_private=$back_private)"
fi

pn_reset
ktap_finished
