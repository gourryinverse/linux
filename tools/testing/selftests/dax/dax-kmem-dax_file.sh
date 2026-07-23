#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# dax/kmem "dax_file=" anonymous-fault cdev on a PUBLIC (non-private) node.
# Independent of "private": mmap()ing /dev/daxN.N yields an ordinary anonymous
# mapping bound to the device's node.
#
#   1. dax_file mmap on a public kmem node places all faulted pages on the node.
#   2. the dax_file cdev rejects MAP_SHARED.
#
# Needs a kmem-bindable dax device on a memoryless node; SKIPs otherwise.
# See private_node_common.sh for memmap= provisioning.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_tool
pn_provision			# locate + bind a kmem dax device on a memoryless node

# Online as a PUBLIC node (mm_capabilities=ALL) with the dax-file cdev present.
pn_hotplug unplugged
echo "$PN_CAP_ALL" > "$D/mm_capabilities" 2>/dev/null
echo 1 > "$D/dax_file" 2>/dev/null
pn_hotplug online_movable
if ! node_in_mask "$PN" has_memory || pn_is_private; then
	ktap_skip_all "could not online $PN as a public dax_file node (state=$(pn_state))"
	pn_reset; exit "$KSFT_SKIP"
fi
ktap_print_msg "public node $PN on $DAX (private=0, dax_file=1)"
ktap_set_plan 2

# 1. dax_file mmap places anonymous pages on the public node
out=$("$TOOL" map "/dev/$DAX" 16 "$PN"); rc=$?
total=$(echo "$out" | pn_field total_pages)
onnode=$(echo "$out" | pn_field "on_node$PN")
if [ "$rc" = "$KSFT_SKIP" ]; then
	ktap_test_skip "mmap/fault unavailable: $out"
elif [ -n "$total" ] && [ "$total" -gt 0 ] && [ "$onnode" = "$total" ]; then
	ktap_test_pass "dax_file mmap placed all $total pages on public node $PN"
else
	ktap_test_fail "dax_file mmap off-node on public node (on_node$PN=$onnode of $total)"
fi

# 2. MAP_SHARED is rejected by the dax_file cdev
out=$("$TOOL" shared "/dev/$DAX")
if echo "$out" | grep -q 'shared_mmap=rejected'; then
	ktap_test_pass "dax_file cdev rejects MAP_SHARED ($out)"
else
	ktap_test_fail "MAP_SHARED not rejected ($out)"
fi

pn_hotplug unplugged 2>/dev/null
echo 0 > "$D/dax_file" 2>/dev/null
pn_reset
ktap_finished
