#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM write-protection leak battery.
#
# Drives code paths that are suspected to write a CRAM folio in place (without
# first promoting it off the read-only tier).  Each subtest forces a folio onto
# the CRAM private node and then checks, from userspace, that a write either
# faults+promotes (pfn changes) or never lands on the CRAM device.  The kernel
# contract verifier (CONFIG_CRAM_DEBUG) is a secondary signal; the dmesg scan
# below treats ANY splat as failure.
#
#   aio_ring               aio completion ring migrated onto CRAM.
#   anon_exclusive_assert  defense-in-depth: a CRAM anon folio promotes on write.
#   shmem_excluded         shmem (tmpfs/memfd) is no longer CRAM-eligible.
#
# Provision a dax device on a memoryless node via memmap= (see vng.workflow).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
TOOL="$DIR/cram_leak_tool"

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -d "$CRAM_DBG" ] && [ -e "$CRAM_DBG/demote_count" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_leak_tool not built"; exit "$KSFT_SKIP"; }

# Find a memoryless dax device, bind to kmem, online in cram mode.
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

# Reclaim of shmem folios needs swap active; cram diverts them before swap.
pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available (pass a raw drive to vng)"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
node_in_mask "$PN" has_private_memory ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online"
trap 'echo unplugged > "$D/state" 2>/dev/null; pn_restore_globals' EXIT

ktap_set_plan 3

# run_sub NAME DESC -- run one subtest; pass iff rc==0 AND no dmesg splat.
run_sub() {
	local name=$1 desc=$2 rc out splat

	dmesg -C 2>/dev/null
	"$TOOL" "$PN" "$name" 2>&1 | tee "/tmp/cram_leak.$name.out"
	rc=${PIPESTATUS[0]}
	out=$(cat "/tmp/cram_leak.$name.out")
	# Kernel contract tripwires: the CONFIG_CRAM_DEBUG verifier, the
	# folio_mark_dirty WARN, plus any generic splat.
	splat=$(dmesg 2>/dev/null | grep -ciE "KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:|modified in place without promotion")

	if [ "$rc" = 3 ]; then
		ktap_test_skip "$desc (environmental): $out"
	elif [ "$rc" = 0 ] && [ "$splat" = 0 ]; then
		ktap_test_pass "$desc"
	elif [ "$splat" != 0 ]; then
		ktap_test_fail "$desc: CRAM contract splat (count=$splat): $out"
	else
		ktap_test_fail "$desc (rc=$rc): $out"
	fi
}

run_sub aio_ring             "aio completion ring must never be migrated onto CRAM"
run_sub anon_exclusive_assert "CRAM anon folio must promote on a FOLL_WRITE access"
run_sub shmem_excluded        "shmem (tmpfs/memfd) must never be demoted onto CRAM"

ktap_finished
