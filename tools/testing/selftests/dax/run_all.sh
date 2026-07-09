#!/bin/bash
# vng batch runner: run the full dax/private-node suite, capture to tmpfs,
# then dump between sentinels (console output garbles; the cat is sequential).
cd "$(dirname "$0")" || exit 1
O=/tmp/o; : > "$O"
TESTS="dax-kmem-hotplug.sh dax-kmem-dax_file.sh
       private_node_isolation.sh private_node_hotplug.sh
       private_node_hotplug_transitions.sh private_node_mbind.sh
       private_node_mempolicy.sh private_node_mempolicy_rebind.sh
       private_node_move_pages.sh private_node_migrate_pages.sh
       private_node_demotion.sh private_node_reclaim.sh
       private_node_compaction.sh private_node_thp.sh
       private_node_pagetables.sh private_node_adistance.sh
       private_node_ltpin.sh private_node_observability.sh
       private_node_pressure.sh private_node_cpuset_default_open.sh
       cram_node_basic.sh cram_readable.sh cram_pgcache.sh cram_coherence.sh cram_pgcache_large.sh cram_readahead.sh
       cram_swap.sh cram_ops.sh cram_fork.sh
       cram_leak.sh cram_pressure.sh cram_perf.sh cram_ratio.sh cram_gate.sh cram_hot.sh cram_trim.sh cram_control.sh"
for t in $TESTS; do
    echo "########## $t ##########" >>"$O"
    timeout 300 bash "./$t" >>"$O" 2>&1
    echo "## $t rc=$? ##" >>"$O"
    # Release page cache (and the resident CRAM file tier, which is ordinary
    # page cache) between tests, so a CRAM node freed of one test's file folios
    # is available to the next -- isolates cross-test node-occupancy effects.
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
done
echo "===O-BEGIN==="
cat "$O"
echo "===O-END==="
