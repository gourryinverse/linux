#!/bin/bash
# In-guest runner for the CRAM adversarial campaign: bug repros, pressure/
# deadlock stress, and the perf benchmark.  Console garbles under load, so fence
# the output with sentinels and extract on the host.  A full dmesg dump follows
# so the host can scan the ENTIRE log for splats.
D="$(dirname "$(readlink -f "$0")")"
echo "===CRAM-BEGIN==="
uname -r
for t in cram_leak.sh cram_pressure.sh cram_perf.sh; do
	echo "########## $t ##########"
	timeout 300 bash "$D/$t" 2>&1
	echo "## $t rc=$? ##"
	echo "---DMESG-AFTER $t---"
	dmesg 2>/dev/null | grep -iE "WARNING|BUG:|modified in place|call trace|RIP:|cram|folio_mark_dirty|truncate_inode|shmem|kasan|refcount" | tail -40
	echo "---DMESG-END $t---"
done
echo "===CRAM-END==="
