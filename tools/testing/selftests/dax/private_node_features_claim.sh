#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# A node's feature mask is claimed by the memory added to it and is fixed for
# as long as the node holds any.
#
# node_memory_features_register() accepts a second range on a node only when it
# names the mask the node already carries, and drops the claim when the node's
# last memory is removed.  Without that, a second device could quietly rewrite
# the mask of a node another device is already using -- turning a private node
# public underneath its owner.
#
#   1. the first device's mask becomes the node's mask
#   2. a second device declaring the SAME mask is accepted
#   3. a device declaring a DIFFERENT mask is refused; the mask is unchanged
#   4. once every range is removed the node takes the different mask
#
# dax_test is the only provider that declares a mask, so it is the only way to
# put two masks on one node.  It needs a physical range to carve and a node to
# hand it to, which the harness supplies:
#
#   DAX_TEST_RANGE_START=<pa> DAX_TEST_RANGE_SIZE=<len> DAX_TEST_NODE=<nid>
#
# SKIPs unless all three are set, since a wrong range would hotplug memory the
# rest of the system is using.
#
# Cell 3 is the discriminating one.  Cell 4 states the contract but does not by
# itself prove the claim was dropped: a node that goes fully offline has its
# pgdat reinitialised on the way back in (hotadd_init_pgdat), which resets the
# mask regardless.  The claim only outlives its memory on a node that stays
# online, which this topology has no way to build.

DIR="$(dirname "$(readlink -f "$0")")"
# shellcheck disable=SC1091
. "$DIR"/../kselftest/ktap_helpers.sh
# shellcheck disable=SC1091
. "$DIR"/private_node_common.sh

DRV=/sys/bus/dax/drivers/kmem

# RECLAIM|USER_NUMA and USER_NUMA alone: both legal private masks, and
# different, which is all the test needs.
MASK_A=$(( $(pn__feat_mask reclaim) | $(pn__feat_mask user_numa) ))
MASK_B=$(pn__feat_mask user_numa)

[ -n "${DAX_TEST_RANGE_START:-}" ] && [ -n "${DAX_TEST_RANGE_SIZE:-}" ] &&
[ -n "${DAX_TEST_NODE:-}" ] ||
	{ ktap_skip_all "set DAX_TEST_RANGE_START/_SIZE/_NODE to run"; exit "$KSFT_SKIP"; }

pn_need_debugfs
NID=$DAX_TEST_NODE

# The harness loads dax_test without a range for its anon mapping; reload it as
# a provider offering this node three devices -- two with the same mask, one
# with another.
reload_provider() {
	local d
	# rmmod cannot take memory back that is still online, and a stuck range
	# is unrecoverable, so park every dax_test device first.
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/state" ] || continue
		echo unplugged > "$d/state" 2>/dev/null
		basename "$d" > /sys/bus/dax/drivers/kmem/unbind 2>/dev/null
	done
	sleep 1
	rmmod dax_test 2>/dev/null
	modprobe dax_test "target_node=$NID" \
		"range_start=$DAX_TEST_RANGE_START" \
		"range_size=$DAX_TEST_RANGE_SIZE" \
		"features=$MASK_A,$MASK_A,$MASK_B" 2>/dev/null
}

# dax devices of this provider, in creation order (mask A, A, B)
provider_devs() {
	local d
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		[ "$(cat "$d/target_node" 2>/dev/null)" = "$NID" ] && basename "$d"
	done
}

kmem_bound() {	# $1 = dax device
	[ "$(basename "$(readlink "$DAX_BASE/$1/driver" 2>/dev/null)" 2>/dev/null)" = kmem ]
}

kmem_bind() {
	echo "$1" > "$DRV/bind" 2>/dev/null
	sleep 1
	kmem_bound "$1"
}

kmem_unbind() {
	echo "$1" > "$DRV/unbind" 2>/dev/null
	sleep 1
}

cleanup() {
	local d
	for d in $DEVS; do
		[ -e "$DAX_BASE/$d/state" ] && echo unplugged > "$DAX_BASE/$d/state" 2>/dev/null
		kmem_unbind "$d"
	done
	rmmod dax_test 2>/dev/null
	# Put the provider back the way the harness had it, so later tests get
	# their private node and its declared mask again.
	pn_provider_load 2>/dev/null || modprobe -q dax_test 2>/dev/null
}
trap cleanup EXIT

modprobe -q kmem 2>/dev/null
reload_provider
DEVS=$(provider_devs)
NDEV=$(echo "$DEVS" | grep -c .)
[ "$NDEV" = 3 ] ||
	{ ktap_skip_all "dax_test gave $NDEV devices on node $NID, want 3"; exit "$KSFT_SKIP"; }

# shellcheck disable=SC2086
set -- $DEVS
DEV_A1=$1 DEV_A2=$2 DEV_B=$3
ktap_print_msg "node $NID: $DEV_A1/$DEV_A2 mask $(printf '%#x' "$MASK_A"), $DEV_B mask $(printf '%#x' "$MASK_B")"

# Teach kmem these ids, then park every device so the binds below are ordered.
# Writing new_id also attaches, and $DEV_B's probe is expected to fail there.
for d in $DEVS; do
	echo "$d" > /sys/bus/dax/drivers/device_dax/unbind 2>/dev/null
	echo "$d" > "$DRV/new_id" 2>/dev/null
done
sleep 1
for d in $DEVS; do
	[ -e "$DAX_BASE/$d/state" ] && echo unplugged > "$DAX_BASE/$d/state" 2>/dev/null
	kmem_unbind "$d"
done

ktap_set_plan 4

# 1. the first device's mask becomes the node's mask
if kmem_bind "$DEV_A1"; then
	got=$(pn_node_features "$NID")
	[ "$((got))" = "$MASK_A" ] &&
		ktap_test_pass "node $NID took $DEV_A1's mask $(printf '%#x' "$MASK_A")" ||
		ktap_test_fail "node $NID mask is ${got:-unset}, expected $(printf '%#x' "$MASK_A")"
else
	ktap_test_fail "$DEV_A1 (mask $(printf '%#x' "$MASK_A")) failed to bind"
	ktap_test_skip "no claimed node to add to"
	ktap_test_skip "no claimed node to add to"
	ktap_test_skip "no claimed node to re-claim"
	ktap_finished
	exit 0
fi

# 2. a second range naming the same mask joins the node
if kmem_bind "$DEV_A2"; then
	ktap_test_pass "$DEV_A2 joined node $NID naming the same mask"
else
	ktap_test_fail "$DEV_A2 was refused despite naming the same mask"
fi

# 3. a range naming a different mask is refused, and changes nothing
kmem_bind "$DEV_B" && bound=1 || bound=0
got=$(pn_node_features "$NID")
if [ "$bound" = 0 ] && [ "$((got))" = "$MASK_A" ]; then
	ktap_test_pass "$DEV_B (mask $(printf '%#x' "$MASK_B")) refused; node $NID still $(printf '%#x' "$MASK_A")"
else
	ktap_test_fail "$DEV_B bound=$bound, node $NID mask ${got:-unset} (want refused)"
fi

# 4. with every range gone the node is free to take the other mask
for d in $DEV_A1 $DEV_A2; do
	echo unplugged > "$DAX_BASE/$d/state" 2>/dev/null
	kmem_unbind "$d"
done
if kmem_bind "$DEV_B"; then
	got=$(pn_node_features "$NID")
	[ "$((got))" = "$MASK_B" ] &&
		ktap_test_pass "node $NID re-claimed with $(printf '%#x' "$MASK_B") once its memory was gone" ||
		ktap_test_fail "node $NID mask is ${got:-unset}, expected $(printf '%#x' "$MASK_B")"
else
	ktap_test_fail "$DEV_B still refused after every range was removed"
fi

ktap_finished
