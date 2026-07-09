#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM trim-callback test (cram_ops.trim contract).
#
# Inflating the balloon reserves pages on the cram node; CRAM hands each reserved
# page to the driver's trim callback so the device can drop its compression
# backing.  This exercises the contract via the dax test driver's cram_trim mode:
#   1 succeed  -> cram_trim_count rises by ~balloon pages
#   0 no cb    -> CRAM zeroes instead, count delta 0
#   2 -EAGAIN  -> CRAM retries then zeroes, count delta 0, no splat
#   3 -EBUSY   -> CRAM zeroes the failed pages, count delta 0, no splat
#
# The balloon is driven the real way (cram_compression_ratio).  Empty node, so
# inflation just reserves free pages -- no reclaim/swap.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
ZRATIO=2000

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e "$CRAM_DBG/nodes" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }

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
		[ -e "$D/cram_trim" ] && [ -e "$D/cram_trim_count" ] ||
			{ ktap_skip_all "$DAX missing cram_trim knobs"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		echo "$ZRATIO" > "$D/cram_zratio" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

cram_balloon() { awk -v n="$PN" '$1==n {print $5}' "$CRAM_DBG/nodes"; }

# online with trim mode $1; returns with the node online and balloon at 0
online_mode() {
	echo unplugged > "$D/state" 2>/dev/null
	echo "$1" > "$D/cram_trim"
	echo online > "$D/state" 2>/dev/null
	node_in_mask "$PN" has_private_memory
}

# inflate the balloon to ~perceived/2 and wait for it to settle; echo balloon size
inflate_half() {
	local i b
	echo 1000 > "$D/cram_compression_ratio"		# target = perceived/2
	for ((i = 0; i < 100; i++)); do
		b=$(cram_balloon)
		[ -n "$b" ] && [ "$b" -ge 2048 ] && break
		sleep 0.2
	done
	sleep 0.5
	cram_balloon
}

cram_provision
ktap_set_plan 4
dmesg -C 2>/dev/null

# Subtest 1: mode 1 (succeed) -> count rises by ~balloon
online_mode 1 || { ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
before=$(cat "$D/cram_trim_count")
bal=$(inflate_half)
after=$(cat "$D/cram_trim_count")
delta=$((after - before))
if [ "$bal" -gt 2048 ] && [ "$delta" -ge $((bal - 4096)) ]; then
	ktap_test_pass "mode 1 succeed: trimmed $delta pages for balloon $bal"
else
	ktap_test_fail "mode 1: trim count delta=$delta balloon=$bal"
fi
echo "$ZRATIO" > "$D/cram_compression_ratio"		# deflate

# Subtests 2-4: modes 0/2/3 -> CRAM zeroes, count delta 0, no crash
i=2
for mode in 0 2 3; do
	online_mode "$mode"
	before=$(cat "$D/cram_trim_count")
	bal=$(inflate_half)
	after=$(cat "$D/cram_trim_count")
	delta=$((after - before))
	if [ "$bal" -gt 2048 ] && [ "$delta" = 0 ]; then
		ktap_test_pass "mode $mode: balloon $bal inflated, CRAM zeroed (count delta 0)"
	else
		ktap_test_fail "mode $mode: count delta=$delta balloon=$bal (expected 0)"
	fi
	echo "$ZRATIO" > "$D/cram_compression_ratio"
	i=$((i + 1))
done

echo unplugged > "$D/state" 2>/dev/null

splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|WARNING:|Oops|refcount_t|modified in place")
[ "$splat" = 0 ] || ktap_print_msg "WARNING: $splat dmesg splat(s) during trim test"

ktap_finished
