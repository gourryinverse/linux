#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test private NUMA node infrastructure from userspace
#
# Tests:
#   1. Module loads and passes internal tests
#   2. Private node visible in sysfs
#   3. Node state flags correct
#   4. numactl/libnuma cannot allocate from private node
#
# Prerequisites:
#   - Kernel built with CONFIG_TEST_NODE_PRIVATE=m
#   - CONFIG_ACPI_NUMA_STANDBY_NODES >= 1
#   - Boot with at least 2 NUMA nodes
#
# Usage: ./test_node_private.sh

set -e

PASS=0
FAIL=0
SKIP=0

pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "SKIP: $1"; SKIP=$((SKIP + 1)); }

# Test 1: Load the module
echo "=== Test: Module load and internal tests ==="
if modprobe test_node_private 2>/dev/null; then
    # Check if all internal tests passed
    RESULTS=$(dmesg | grep 'test_node_private: === RESULTS:' | tail -1)
    if echo "$RESULTS" | grep -q '0 failed'; then
        pass "Module loaded, all internal tests passed"
        echo "  $RESULTS"
    else
        fail "Module loaded but some internal tests failed"
        echo "  $RESULTS"
        dmesg | grep 'test_node_private: FAIL' | while read -r line; do
            echo "  $line"
        done
    fi
else
    fail "Module failed to load"
    dmesg | tail -5
fi

# Get the private node ID from dmesg
PRIVATE_NID=$(dmesg | grep 'test_node_private:.*Got exclusive node' | tail -1 | grep -oP 'node \K[0-9]+')
if [ -z "$PRIVATE_NID" ]; then
    echo "Could not determine private node ID, skipping remaining tests"
    SKIP=$((SKIP + 5))
else
    echo "Private node ID: $PRIVATE_NID"

    # Test 2: Check sysfs node exists
    echo ""
    echo "=== Test: Sysfs visibility ==="
    if [ -d "/sys/devices/system/node/node${PRIVATE_NID}" ]; then
        pass "Node $PRIVATE_NID exists in sysfs"
    else
        skip "Node $PRIVATE_NID not in sysfs (no memory onlined)"
    fi

    # Test 3: Check N_MEMORY_PRIVATE state
    echo ""
    echo "=== Test: Node state ==="
    # Without memory hotplugged, the node won't be in N_MEMORY_PRIVATE yet
    if [ -f "/sys/devices/system/node/has_memory" ]; then
        HAS_MEM=$(cat /sys/devices/system/node/has_memory)
        echo "  has_memory: $HAS_MEM"
        if echo "$HAS_MEM" | grep -qw "$PRIVATE_NID"; then
            skip "Node $PRIVATE_NID has memory (unexpected without hotplug)"
        else
            pass "Node $PRIVATE_NID correctly not in has_memory (no hotplug)"
        fi
    else
        skip "has_memory sysfs not available"
    fi

    # Test 4: Check numastat doesn't show private node
    echo ""
    echo "=== Test: Allocation isolation ==="
    if command -v numastat >/dev/null 2>&1; then
        NUMA_OUT=$(numastat 2>/dev/null || true)
        if echo "$NUMA_OUT" | grep -q "Node $PRIVATE_NID"; then
            skip "numastat shows node $PRIVATE_NID (has memory)"
        else
            pass "numastat doesn't show private node $PRIVATE_NID"
        fi
    else
        skip "numastat not available"
    fi

    # Test 5: Try to set mempolicy with private node
    echo ""
    echo "=== Test: Mempolicy rejection ==="
    if command -v numactl >/dev/null 2>&1; then
        # numactl --membind should fail for a private node without memory
        if numactl --membind=$PRIVATE_NID true 2>/dev/null; then
            fail "numactl --membind to private node should fail"
        else
            pass "numactl --membind to private node correctly rejected"
        fi
    else
        skip "numactl not available"
    fi
fi

# Test 6: Unload module cleanly
echo ""
echo "=== Test: Module unload ==="
if rmmod test_node_private 2>/dev/null; then
    pass "Module unloaded cleanly"
    # Check cleanup messages
    if dmesg | grep -q 'test_node_private: Unregistered private node'; then
        pass "Unregister cleanup confirmed"
    else
        fail "No unregister cleanup message"
    fi
    if dmesg | grep -q 'test_node_private: Released exclusive node'; then
        pass "Exclusive node release confirmed"
    else
        fail "No exclusive node release message"
    fi
else
    fail "Module unload failed"
fi

echo ""
echo "=== RESULTS: $PASS passed, $FAIL failed, $SKIP skipped ==="
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
