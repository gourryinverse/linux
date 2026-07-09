#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM proactive-promotion test (cram_report_hot_pages).
#
# Demotes an anon region onto the cram node, then reports its pfns as hot through
# the dax cram_hot_pages knob and asserts CRAM promotes the folios back to DRAM
# (the inverse of demotion): the pages leave the cram node, promote_count rises,
# and the data stays readable throughout.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
TOOL="$DIR/cram_hot_tool"

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e "$CRAM_DBG/promote_count" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_hot_tool not built"; exit "$KSFT_SKIP"; }

cram_provision() {
	local d nid drv r

	modprobe -q nd_e820 dax_pmem device_dax nd_pmem 2>/dev/null
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
		D=$d; DAX=$(basename "$d"); PN=$nid
		[ -e "$D/cram_hot_pages" ] ||
			{ ktap_skip_all "$DAX has no cram_hot_pages knob"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

cram_provision
echo online > "$D/state" 2>/dev/null
node_in_mask "$PN" has_private_memory ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online"
trap 'echo unplugged > "$D/state" 2>/dev/null' EXIT

ktap_set_plan 2

dmesg -C 2>/dev/null
out=$("$TOOL" "$PN" "$D/cram_hot_pages"); rc=$?
case "$rc" in
0) ktap_test_pass "device-reported hot pages promoted off cram to DRAM [$out]" ;;
3) ktap_test_skip "could not demote onto cram (environmental) [$out]" ;;
*) ktap_test_fail "hot pages not promoted (rc=$rc) [$out]" ;;
esac

splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|WARNING:|Oops|refcount_t|modified in place")
if [ "$splat" = 0 ]; then
	ktap_test_pass "no splat during proactive promotion"
else
	ktap_test_fail "kernel splat during proactive promotion (count=$splat)"
fi

ktap_finished
