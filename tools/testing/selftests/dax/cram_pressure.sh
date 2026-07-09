#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CRAM read-only tier pressure / livelock test.
#
# Hammers the CRAM tier with simultaneous demotion pressure and promotion
# storms (read-fault + mmap-write faults + fork teardown) against on-cram anon
# folios, trying to wedge the machine (livelock / deadlock / OOM)
# or trip a write-protection splat.  A forward-progress watchdog inside the tool
# catches a livelock (the box is alive but no op completes); the pfn-must-change-
# on-write check catches an in-place write leak the CONFIG_CRAM_DEBUG verifier
# misses for writable PTEs; the dmesg scan below catches splats.
#
# Stress duration is tunable via CRAM_STRESS_SECS (default modest so the suite's
# 300s timeout is respected).
#
# Provision a dax device on a memoryless node via memmap= (see vng.workflow).
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

CRAM_DBG=/sys/kernel/debug/cram
TOOL="$DIR/cram_pressure_tool"

ktap_print_header
pn_require_root

grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -d "$CRAM_DBG" ] && [ -e "$CRAM_DBG/demote_count" ] ||
	{ ktap_skip_all "cram debugfs missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -e "$CRAM_DBG/promote_count" ] ||
	{ ktap_skip_all "cram promote_count missing (CONFIG_CRAM=y?)"; exit "$KSFT_SKIP"; }
[ -x "$TOOL" ] ||
	{ ktap_skip_all "cram_pressure_tool not built"; exit "$KSFT_SKIP"; }

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

# Reclaim of shmem folios needs swap active; cram diverts them before swap, and
# resident cram folios spill to physical swap once the node fills under pressure.
pn_snapshot_swaps
pn_swap_setup ||
	{ ktap_skip_all "no swap device available (pass a raw drive to vng)"; exit "$KSFT_SKIP"; }

cram_provision
echo online > "$D/state" 2>/dev/null
node_in_mask "$PN" has_private_memory ||
	{ ktap_skip_all "could not online cram node $PN"; exit "$KSFT_SKIP"; }
ktap_print_msg "cram node $PN online"
trap 'echo unplugged > "$D/state" 2>/dev/null; pn_restore_globals' EXIT

ktap_set_plan 6

# scan the ENTIRE dmesg for any wedge/leak tripwire after a subtest.
scan_splat() {
	dmesg 2>/dev/null | grep -ciE \
		"KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:|hung task|rcu_sched|soft lockup|modified in place without promotion"
}

# run_sub NAME DESC -- run one subtest, scan dmesg, emit two ktap results
# (the functional/livelock result and the splat result).
run_sub() {
	local name=$1 desc=$2 rc out splat

	dmesg -C 2>/dev/null
	"$TOOL" "$PN" "$name" 2>&1 | tee "/tmp/cram_pressure_$name.out"
	rc=${PIPESTATUS[0]}
	out=$(cat "/tmp/cram_pressure_$name.out")
	splat=$(scan_splat)
	if [ "$splat" != 0 ]; then
		dmesg 2>/dev/null | grep -iE \
			"KASAN|BUG:|use-after-free|general protection|Oops|refcount_t|WARNING:|hung task|rcu_sched|soft lockup|modified in place without promotion" \
			| sed 's/^/# SPLAT['"$name"']: /'
		dmesg 2>/dev/null | grep -A12 -iE "WARNING:|BUG:" | sed 's/^/# TRACE: /' | head -40
	fi

	if [ "$rc" = 0 ]; then
		ktap_test_pass "$desc"
	elif [ "$rc" = 3 ]; then
		ktap_test_skip "$desc: environmental (no demote/promote progress): $out"
	else
		ktap_test_fail "$desc (rc=$rc): $out"
	fi

	if [ "$splat" = 0 ]; then
		ktap_test_pass "no wedge/leak splat during $name"
	else
		ktap_test_fail "kernel splat during $name (count=$splat)"
	fi
}

# 1+2. full concurrent storm + forward-progress watchdog + counter-delta coverage
run_sub stress "CRAM pressure storm: no livelock/leak, demote+promote advanced"

# 3+4. simultaneous first-write race (isolate/-EAGAIN retry convergence)
run_sub promote_race "promote race: all racing writes land + promoted (pfn moved)"

# 5+6. fresh-demoted non-LRU folio write -> lru_add_drain escalation in promote
run_sub drain_race "drain race: write to just-demoted (non-LRU) folio promotes"

ktap_finished
