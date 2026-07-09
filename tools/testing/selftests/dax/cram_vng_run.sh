#!/bin/bash
# In-guest runner for the cram selftests under vng: console garbles under load,
# so fence the output with sentinels and extract on the host.
D="$(dirname "$(readlink -f "$0")")"
echo "===CRAM-BEGIN==="
uname -r
for t in cram_node_basic.sh cram_readable.sh cram_swap.sh cram_ops.sh cram_fork.sh cram_leak.sh cram_pressure.sh cram_perf.sh cram_ratio.sh cram_gate.sh cram_hot.sh cram_trim.sh cram_control.sh; do
	echo "########## $t ##########"
	bash "$D/$t" 2>&1
	echo "## $t rc=$? ##"
done
echo "===CRAM-END==="
