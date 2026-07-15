#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# cram_fs_matrix.sh - run the CRAM file-tier correctness tests across every
# filesystem in $CRAM_FS_LIST, on a single raw data disk ($CRAM_FS_DISK).  The
# test plan's VM lane wants the file-tier covered on all in-tree filesystems;
# each underlying test mkfs+mounts the disk itself, so this just drives CRAM_FS.
#
#   CRAM_FS_LIST  filesystems to sweep       (default "ext4 xfs btrfs")
#   CRAM_FS_DISK  raw data disk, WILL BE MKFS'd (default /dev/vda)
#
# The file-tier tests currently support ext4/xfs in their mkfs case; a filesystem
# the test can't format SKIPs cleanly (KSFT_SKIP), so an unsupported entry in the
# list shows up as SKIP rather than breaking the sweep.  Adding a filesystem =
# extend the mkfs case in each FILE_TESTS script below.
#
# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh

FS_LIST=${CRAM_FS_LIST:-"ext4 xfs btrfs"}
DISK=${CRAM_FS_DISK:-/dev/vda}

FILE_TESTS="cram_pgcache.sh cram_pgcache_large.sh cram_readahead.sh cram_coherence.sh"

ktap_print_header
[ "$(id -u)" = 0 ] || { ktap_skip_all "must run as root"; exit "$KSFT_SKIP"; }
[ -b "$DISK" ] || { ktap_skip_all "CRAM_FS_DISK=$DISK is not a block device"; exit "$KSFT_SKIP"; }

# One plan point per (fs, test) cell.
n=0; for fs in $FS_LIST; do for t in $FILE_TESTS; do n=$((n+1)); done; done
ktap_set_plan "$n"

fail=0
for fs in $FS_LIST; do
	for t in $FILE_TESTS; do
		out=$(CRAM_FS="$fs" CRAM_FS_DISK="$DISK" timeout 300 bash "$DIR/$t" 2>&1)
		rc=$?
		if [ "$rc" = 0 ]; then
			ktap_test_pass "$fs/$t"
		elif [ "$rc" = "$KSFT_SKIP" ]; then
			# surface why (e.g. "unsupported CRAM_FS", "mkfs.$fs unavailable")
			why=$(printf '%s\n' "$out" | grep -m1 -iE "skip|unsupported|unavailable|failed" | tail -c 80)
			ktap_test_skip "$fs/$t ${why:+- $why}"
		else
			ktap_test_fail "$fs/$t (rc=$rc)"
			printf '%s\n' "$out" | tail -5 | sed 's/^/    # /'
			fail=1
		fi
	done
done

ktap_finished
exit "$fail"
