#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM dynamic-capacity (compression ratio -> balloon) test.
#
# Drives the driver directive cram_set_compression_ratio() through the dax
# device's cram_compression_ratio sysfs knob and asserts the balloon resizes:
# a reported ratio below the configured zratio inflates the balloon (shrinking
# EFFECTIVE capacity to fit the device's real physical backing); restoring the
# ratio deflates it.  State is read from /sys/kernel/debug/cram/nodes.
#
# No swap needed: the node is empty here, so balloon inflation just reserves the
# node's own free pages (no reclaim).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
ZRATIO=2000		# 2:1 configured

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -e "$CRAM_DBG/nodes" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }

# Find a memoryless dax device, bind kmem, configure zratio, online as cram.
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
		[ -e "$D/cram" ] && [ -e "$D/cram_zratio" ] && [ -e "$D/cram_compression_ratio" ] ||
			{ ktap_skip_all "$DAX missing cram ratio knobs"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		echo "$ZRATIO" > "$D/cram_zratio" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

# cram_field COL -> value of column COL (1-based) for node PN from cram/nodes
# columns: 1 node 2 zratio 3 current_ratio 4 perceived 5 balloon 6 target 7 blocked
cram_field() { awk -v n="$PN" -v c="$1" '$1==n {print $c}' "$CRAM_DBG/nodes"; }

# poll_balloon CMP THRESH SECS -- wait until balloon CMP THRESH (e.g. -ge 4096)
poll_balloon() {
	local cmp=$1 thr=$2 secs=$3 i b
	for ((i = 0; i < secs * 5; i++)); do
		b=$(cram_field 5)
		[ -n "$b" ] && [ "$b" "$cmp" "$thr" ] && return 0
		sleep 0.2
	done
	return 1
}

cram_provision
echo online > "$D/state" 2>/dev/null
node_in_mask "$PN" has_private_memory ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online, zratio=$(cram_field 2) perceived=$(cram_field 4)"
trap 'echo unplugged > "$D/state" 2>/dev/null' EXIT

ktap_set_plan 3

dmesg -C 2>/dev/null
PERCEIVED=$(cram_field 4)
CHUNK=2048			# CRAM_WMARK_CHUNK (SZ_8M / PAGE_SIZE)

# 1. baseline: current_ratio == zratio -> target/balloon ~0
base_t=$(cram_field 6); base_b=$(cram_field 5)
if [ "$base_t" = 0 ] && [ "$base_b" -le "$CHUNK" ]; then
	ktap_test_pass "baseline: balloon empty at configured ratio (target=$base_t balloon=$base_b)"
else
	ktap_test_fail "baseline balloon not empty (target=$base_t balloon=$base_b)"
fi

# 2. worse ratio -> balloon inflates toward perceived*(zratio-ratio)/zratio
echo 1500 > "$D/cram_compression_ratio"
want=$(cram_field 6)		# target after the directive (immediate)
if [ -n "$want" ] && [ "$want" -gt "$CHUNK" ] && poll_balloon -ge "$CHUNK" 30; then
	ktap_test_pass "worse ratio 1500 inflates balloon (target=$want balloon=$(cram_field 5))"
else
	ktap_test_fail "balloon did not inflate (target=$want balloon=$(cram_field 5))"
fi

# 3. restore ratio -> balloon deflates back toward 0
echo "$ZRATIO" > "$D/cram_compression_ratio"
if [ "$(cram_field 6)" = 0 ] && poll_balloon -le "$CHUNK" 30; then
	ktap_test_pass "restored ratio deflates balloon (balloon=$(cram_field 5))"
else
	ktap_test_fail "balloon did not deflate (target=$(cram_field 6) balloon=$(cram_field 5))"
fi

splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|WARNING:|Oops|refcount_t|modified in place")
[ "$splat" = 0 ] || ktap_print_msg "WARNING: $splat dmesg splat(s) during ratio test"

ktap_finished
