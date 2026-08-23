#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# A node may withhold NODE_MEMORY_FEAT_USER_WRITE with no help from its
# provider.
#
# The fence has two halves and both are generic.  Enforcement maps the node's
# folios read-only wherever node_write_fenced() is consulted.  Relief moves a
# folio the caller must write onto a public node: anon through the core COW
# path, page cache through promote_fenced_folio().  Neither asks who owns the
# node, so withholding the bit is something any provider can do.
#
# That is the property worth testing, because it is the one that is easy to
# lose.  If relief were ever keyed on a particular service instead of the node
# feature, a node fenced by anyone else would be write-protected with nothing
# able to move a folio off it, and the shared write fault would fall through
# to finish_mkwrite_fault(), make the PTE writable again and silently drop the
# fence.
#
# dax_test supplies nothing but a feature mask -- no promote path of its own --
# which is exactly what makes it the right provider here:
#
#   1. a mask withholding USER_WRITE is ACCEPTED from it, and the node comes up
#      private and fenced
#   2. granting USER_WRITE still works, so acceptance is not vacuous
#
# The fence's behaviour on a populated node -- reads served in place, writes
# promoting off -- is covered by the cram_* tests.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

pn_begin
pn_require_root

ktap_set_plan 2

if [ -z "${PN_DAX_TEST_RANGE_START:-}" ]; then
	ktap_skip_all "no carved range (set PN_DAX_TEST_RANGE_START/_SIZE/_NODE)"
	exit "$KSFT_SKIP"
fi

NID=${PN_DAX_TEST_NODE}

# ---------------------------------------------------------------------------
# 1. fenced, from a provider with no promote path of its own
# ---------------------------------------------------------------------------
# RECLAIM|USER_NUMA, no USER_WRITE.
modprobe -r dax_test 2>/dev/null
PN_DAX_TEST_FEATURES=0x82 pn_provider_load
if pn_node_is_private "$NID"; then
	ktap_test_pass "1 node $NID came up fenced with no provider-side promoter"
else
	ktap_test_fail "1 fenced mask refused: relief is not generic"
fi

# ---------------------------------------------------------------------------
# 2. granting USER_WRITE also works (acceptance above is not vacuous)
# ---------------------------------------------------------------------------
modprobe -r dax_test 2>/dev/null
PN_DAX_TEST_FEATURES=0x182 pn_provider_load
if pn_node_is_private "$NID"; then
	ktap_test_pass "2 node $NID came up private when granting USER_WRITE"
else
	ktap_test_fail "2 node $NID refused even with USER_WRITE granted"
fi

ktap_finished
