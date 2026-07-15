#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM file-tier latency probe runner (perf; the file-tier companion to
# cram_perf.sh, which measures the anon paths).
#
# Provisions a CRAM node, enables demotion, puts a test file on a real
# filesystem, and runs cram_file_perf_tool to time the read-only-tier file
# access costs (in-place read, COW-promote write, cross-process read).
#
# Needs a real FS for the file: CRAM_FS_DIR (a mounted dir) OR CRAM_FS_DISK (a
# raw disk this script mkfs+mounts, default /dev/vdb).  SKIPs if neither works.
# Env: CRAM_FILE_MB (default 64).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
TOOL="$DIR/cram_file_perf_tool"
MB=${CRAM_FILE_MB:-64}

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -d "$CRAM_DBG" ] && [ -e "$CRAM_DBG/demote_count" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$TOOL" ] || { ktap_skip_all "cram_file_perf_tool not built"; exit "$KSFT_SKIP"; }

# Find a memoryless dax device, bind to kmem, online in cram mode.
cram_provision() {
	local d nid drv r

	modprobe -q nd_e820 dax_pmem device_dax nd_pmem 2>/dev/null
	modprobe -q kmem 2>/dev/null
	[ -d /sys/bus/dax/drivers/kmem ] ||
		{ ktap_skip_all "kmem driver unavailable"; exit "$KSFT_SKIP"; }
	if command -v ndctl >/dev/null 2>&1; then
		for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
			ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f >/dev/null 2>&1
		done
	fi
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node"); [ "$nid" -ge 0 ] 2>/dev/null || continue
		node_in_mask "$nid" has_memory && continue
		drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
		[ "$drv" = device_dax ] && basename "$d" > /sys/bus/dax/drivers/device_dax/unbind 2>/dev/null
		[ "$drv" = kmem ] || basename "$d" > /sys/bus/dax/drivers/kmem/new_id 2>/dev/null
		sleep 1
		D=$d; DAX=$(basename "$d"); PN=$nid
		[ -e "$D/cram" ] || { ktap_skip_all "$DAX has no 'cram' knob"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node"
	exit "$KSFT_SKIP"
}

# real FS for the file: prefer CRAM_FS_DIR, else mkfs+mount CRAM_FS_DISK
MNT=""; CLEAN_MNT=0
fs_setup() {
	if [ -n "${CRAM_FS_DIR:-}" ] && [ -d "$CRAM_FS_DIR" ]; then MNT=$CRAM_FS_DIR; return 0; fi
	local disk=${CRAM_FS_DISK:-/dev/vdb}
	[ -b "$disk" ] || return 1
	command -v mkfs.ext4 >/dev/null 2>&1 || return 1
	mkfs.ext4 -qF "$disk" >/dev/null 2>&1 || return 1
	MNT=$(mktemp -d); mount "$disk" "$MNT" 2>/dev/null || return 1
	CLEAN_MNT=1; return 0
}

pn_snapshot_swaps
pn_swap_setup || { ktap_skip_all "no swap device available"; exit "$KSFT_SKIP"; }
fs_setup || { ktap_skip_all "no real FS (set CRAM_FS_DIR or pass CRAM_FS_DISK)"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
node_in_mask "$PN" has_private_memory ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
echo true > /sys/kernel/mm/numa/demotion_enabled 2>/dev/null
ktap_print_msg "cram node $PN online; file FS at $MNT"
trap 'echo unplugged > "$D/state" 2>/dev/null; [ "$CLEAN_MNT" = 1 ] && umount "$MNT" 2>/dev/null; pn_restore_globals' EXIT

ktap_set_plan 1
echo "# --- cram file-tier latency (node $PN, ${MB}MB) ---"
"$TOOL" "$PN" "$MNT/cram_file_perf.dat" "$MB"; rc=$?
if [ "$rc" = 0 ]; then
	ktap_test_pass "cram file-tier latency probe"
elif [ "$rc" = 3 ]; then
	ktap_test_skip "file did not demote to CRAM (environmental)"
else
	ktap_test_fail "cram file-tier probe (rc=$rc)"
fi

ktap_finished
