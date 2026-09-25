#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM dynamic-capacity (compression ratio -> balloon) test.
#
# Drives the dax provider's compression_ratio knob and asserts the balloon
# resizes through cram_set_capacity():
# a reported ratio below the configured zratio inflates the balloon (shrinking
# EFFECTIVE capacity to fit the device's real physical backing); restoring the
# ratio deflates it.  Balloon accounting comes from the node's vmstat.
#
# No swap needed: the node is empty here, so balloon inflation just reserves the
# node's own free pages (no reclaim).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

ZRATIO=2000		# 2:1 configured

ktap_print_header
pn_require_root

# Find a memoryless dax device, bind cramdax, configure zratio, and online.
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
		[ -e "$D/state" ] && [ -e "$D/zratio" ] && [ -e "$D/compression_ratio" ] ||
			{ ktap_skip_all "$DAX missing cram ratio knobs"; exit "$KSFT_SKIP"; }
		echo offline > "$D/state" 2>/dev/null
		echo "$ZRATIO" > "$D/zratio" 2>/dev/null
		return 0
	done
	ktap_skip_all "no cramdax-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

cram_balloon() { pn_node_vmstat "$PN" nr_balloon_pages; }

# poll_balloon CMP THRESH SECS -- wait until balloon CMP THRESH (e.g. -ge 4096)
poll_balloon() {
	local cmp=$1 thr=$2 secs=$3 i b
	for ((i = 0; i < secs * 5; i++)); do
		b=$(cram_balloon)
		[ -n "$b" ] && [ "$b" "$cmp" "$thr" ] && return 0
		sleep 0.2
	done
	return 1
}

cram_provision
echo online > "$D/state" 2>/dev/null
pn_node_is_private "$PN" ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online, zratio=$(cat "$D/zratio")"
trap 'echo offline > "$D/state" 2>/dev/null' EXIT

ktap_set_plan 3

dmesg -C 2>/dev/null
CHUNK=2048			# CRAM_WMARK_CHUNK (SZ_8M / PAGE_SIZE)

# 1. baseline: achieved ratio equals zratio, so the balloon is empty
base_b=$(cram_balloon)
if [ "$base_b" -le "$CHUNK" ]; then
	ktap_test_pass "baseline: balloon empty at configured ratio (balloon=$base_b)"
else
	ktap_test_fail "baseline balloon not empty (balloon=$base_b)"
fi

# 2. worse ratio -> balloon inflates toward perceived*(zratio-ratio)/zratio
echo 1500 > "$D/compression_ratio"
if poll_balloon -ge "$CHUNK" 30; then
	ktap_test_pass "worse ratio 1500 inflates balloon (balloon=$(cram_balloon))"
else
	ktap_test_fail "balloon did not inflate (balloon=$(cram_balloon))"
fi

# 3. restore ratio -> balloon deflates back toward 0
echo "$ZRATIO" > "$D/compression_ratio"
if poll_balloon -le "$CHUNK" 30; then
	ktap_test_pass "restored ratio deflates balloon (balloon=$(cram_balloon))"
else
	ktap_test_fail "balloon did not deflate (balloon=$(cram_balloon))"
fi

splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|WARNING:|Oops|refcount_t|modified in place")
[ "$splat" = 0 ] || ktap_print_msg "WARNING: $splat dmesg splat(s) during ratio test"

ktap_finished
