#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM trim-callback test (cram_ops.trim contract).
#
# Inflating the balloon reserves pages on the cram node; CRAM hands each reserved
# page to the driver's trim callback so the device can drop its compression
# backing.  cramdax implements that callback by zeroing each page and counts
# the completed pages in trim_count.
#
# The balloon is driven through compression_ratio.  Empty node, so
# inflation just reserves free pages -- no reclaim/swap.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

ZRATIO=2000

ktap_print_header
pn_require_root

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
		D=$d; DAX=$(basename "$d"); PN=$nid
		[ -e "$D/trim_count" ] ||
			{ ktap_skip_all "$DAX missing trim_count"; exit "$KSFT_SKIP"; }
		echo offline > "$D/state" 2>/dev/null
		echo "$ZRATIO" > "$D/zratio" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

cram_balloon() { pn_node_vmstat "$PN" nr_balloon_pages; }

# inflate the balloon to ~perceived/2 and wait for it to settle; echo balloon size
inflate_half() {
	local i b
	echo 1000 > "$D/compression_ratio"		# target = perceived/2
	for ((i = 0; i < 100; i++)); do
		b=$(cram_balloon)
		[ -n "$b" ] && [ "$b" -ge 2048 ] && break
		sleep 0.2
	done
	sleep 0.5
	cram_balloon
}

cram_provision
ktap_set_plan 1
dmesg -C 2>/dev/null

echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
before=$(cat "$D/trim_count")
bal=$(inflate_half)
after=$(cat "$D/trim_count")
delta=$((after - before))
if [ "$bal" -gt 2048 ] && [ "$delta" -ge $((bal - 4096)) ]; then
	ktap_test_pass "cramdax zeroed $delta pages for balloon $bal"
else
	ktap_test_fail "trim count delta=$delta balloon=$bal"
fi
echo "$ZRATIO" > "$D/compression_ratio"		# deflate

echo offline > "$D/state" 2>/dev/null

splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|WARNING:|Oops|refcount_t|modified in place")
[ "$splat" = 0 ] || ktap_print_msg "WARNING: $splat dmesg splat(s) during trim test"

ktap_finished
