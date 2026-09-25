#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Validate the userspace contracts around an anondax private node.

# shellcheck disable=SC1091
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR"/../kselftest/ktap_helpers.sh
. "$DIR"/private_node_common.sh

TOOL="$DIR/anondax_policy_tool"

ktap_print_header
pn_require_root
[ -x "$TOOL" ] ||
	{ ktap_skip_all "anondax_policy_tool not built"; exit "$KSFT_SKIP"; }

pn_modprobe nd_e820 dax_pmem device_dax nd_pmem anondax
if command -v ndctl >/dev/null 2>&1; then
	for region in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
		ndctl create-namespace -m devdax \
			-e "${region/region/namespace}.0" -f >/dev/null 2>&1
	done
fi

for D in "$DAX_BASE"/dax*; do
	[ -e "$D/target_node" ] || continue
	PN=$(cat "$D/target_node")
	[ "$PN" -ge 0 ] 2>/dev/null || continue
	pn_bind_anondax "$D" || continue
	break
done
[ -n "${D:-}" ] && [ "$(basename "$(readlink "$D/driver")")" = anondax ] || {
	ktap_skip_all "no anondax-bindable device"
	exit "$KSFT_SKIP"
}

for node in "$NODE_BASE"/node[0-9]*; do
	nid=${node##*node}
	node_in_mask "$nid" has_common_memory || continue
	COMMON=$nid
	break
done
[ -n "${COMMON:-}" ] || {
	ktap_skip_all "no common memory node"
	exit "$KSFT_SKIP"
}

cleanup()
{
	echo "$(basename "$D")" > /sys/bus/dax/drivers/anondax/unbind 2>/dev/null
}
trap cleanup EXIT

ktap_set_plan 9
dmesg -C 2>/dev/null
out=$("$TOOL" "/dev/$(basename "$D")" "$PN" "$COMMON"); rc=$?
echo "$out"

for field in default_isolated mems_visible policy_rejected bind_rejected \
		mixed_narrowed target_rejected private_hidden move_blocked lock_stable; do
	if [ "$rc" = 0 ] && grep -q "$field=1" <<<"$out"; then
		ktap_test_pass "$field"
	else
		ktap_test_fail "$field (rc=$rc)"
	fi
done

ktap_finished
