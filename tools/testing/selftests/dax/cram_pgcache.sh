#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM resident (read-in-place) file-cache tier test.
#
# A clean file folio that reclaim would drop is demoted onto the CRAM node by
# migration and stays in the page cache, mapped READ-ONLY in place: an mmap read
# faults it in zero-copy (present, on-cram, pfn unchanged); a byte write
# (MAP_SHARED store or write(2)) promotes it off the tier first (pfn changes, off
# cram) so the read-only device is never written in place.  Needs a real
# (block-backed) filesystem -- shmem/tmpfs is ineligible (no ->read_folio).
#
# Set CRAM_FS=ext4|xfs (default ext4) and CRAM_FS_DISK=/dev/vda (a raw data disk
# passed to vng, distinct from any swap disk).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
TOOL="$DIR/cram_pgcache_tool"
FS=${CRAM_FS:-ext4}
DISK=${CRAM_FS_DISK:-/dev/vda}
MNT=/tmp/crampc	# guest root is read-only; /tmp is a writable tmpfs

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -d "$CRAM_DBG" ] && [ -e "$CRAM_DBG/nodes" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_pgcache_tool not built"; exit "$KSFT_SKIP"; }
command -v "mkfs.$FS" >/dev/null 2>&1 ||
	{ ktap_skip_all "mkfs.$FS unavailable"; exit "$KSFT_SKIP"; }
[ -b "$DISK" ] ||
	{ ktap_skip_all "$DISK is not a block device (pass a raw drive to vng)"; exit "$KSFT_SKIP"; }

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
		[ -e "$D/cram" ] ||
			{ ktap_skip_all "$DAX has no 'cram' knob"; exit "$KSFT_SKIP"; }
		echo unplugged > "$D/state" 2>/dev/null
		echo 1 > "$D/cram" 2>/dev/null
		return 0
	done
	ktap_skip_all "no kmem-bindable dax device on a memoryless node (see header)"
	exit "$KSFT_SKIP"
}

case "$FS" in
ext4)	mkfs.ext4 -F -q "$DISK" >/dev/null 2>&1 ;;
xfs)	mkfs.xfs -f -q "$DISK" >/dev/null 2>&1 ;;
*)	ktap_skip_all "unsupported CRAM_FS=$FS"; exit "$KSFT_SKIP" ;;
esac || { ktap_skip_all "mkfs.$FS failed on $DISK"; exit "$KSFT_SKIP"; }
mkdir -p "$MNT"
mount "$DISK" "$MNT" ||
	{ ktap_skip_all "mount $DISK failed"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
node_in_mask "$PN" has_private_memory ||
	{ umount "$MNT"; ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online, $FS on $DISK"
trap 'echo unplugged > "$D/state" 2>/dev/null; umount "$MNT" 2>/dev/null' EXIT

ktap_set_plan 2

dmesg -C 2>/dev/null
"$TOOL" "$PN" "$MNT" 2>&1 | tee /tmp/cram_pgcache.out
rc=${PIPESTATUS[0]}
out=$(cat /tmp/cram_pgcache.out)
splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:")

# 1. resident read-in-place + shared-write / write(2) promote-off-cram
if [ "$rc" = 0 ]; then
	ktap_test_pass "resident file tier: mmap read in place, write promotes off CRAM on $FS"
elif [ "$rc" = 3 ]; then
	ktap_test_skip "clean file folios did not go resident on CRAM (environmental): $out"
else
	ktap_test_fail "resident file tier contract failed (rc=$rc): $out"
fi

# 2. no kernel splat during the round trip (the write fence held)
if [ "$splat" = 0 ]; then
	ktap_test_pass "no KASAN/BUG/UAF/WARN during resident file tier round trip"
else
	ktap_test_fail "kernel splat during resident file tier round trip (count=$splat)"
fi

ktap_finished
