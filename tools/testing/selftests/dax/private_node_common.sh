# SPDX-License-Identifier: GPL-2.0
# Shared helpers used by private-node selftests.

DAX_BASE=/sys/bus/dax/devices
NODE_BASE=/sys/devices/system/node

pn_modprobe()
{
	local module

	for module in "$@"; do
		modprobe -q "$module" 2>/dev/null
	done
}

nodelist_has()
{
	local nid=$2 token low high

	for token in $(echo "$1" | tr ',' ' '); do
		low=${token%-*}
		high=${token#*-}
		[ "$nid" -ge "$low" ] 2>/dev/null &&
			[ "$nid" -le "$high" ] 2>/dev/null && return 0
	done
	return 1
}

node_in_mask()
{
	[ -r "$NODE_BASE/$2" ] &&
		nodelist_has "$(cat "$NODE_BASE/$2")" "$1"
}

pn_node_is_private()
{
	node_in_mask "$1" has_private_memory
}

pn_is_private()
{
	pn_node_is_private "$PN"
}

# Online a dax device as private kmem memory that memory policies may
# target. Record the
# device's original binding for pn_unbind_kmem_private().
pn_bind_kmem_private()
{
	local dev=$1 name driver nid

	name=$(basename "$dev")
	nid=$(cat "$dev/target_node" 2>/dev/null) || return 1
	driver=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null)
	if [ -z "${PN_DEV:-}" ]; then
		PN_DEV=$dev
		PN_ORIG_DRIVER=$driver
		PN_ORIG_STATE=$(cat "$dev/state" 2>/dev/null)
		PN_ORIG_PRIVATE=$(cat "$dev/private" 2>/dev/null)
		PN_NEW_ID=
	fi
	if [ "$driver" = kmem ]; then
		if [ "$(cat "$dev/private" 2>/dev/null)" = 1 ] &&
		   pn_node_is_private "$nid"; then
			return 0
		fi
		echo unplugged > "$dev/state" 2>/dev/null || return 1
	fi
	if [ -n "$driver" ] && [ "$driver" != kmem ]; then
		echo "$name" > "/sys/bus/dax/drivers/$driver/unbind" 2>/dev/null ||
			return 1
	fi
	if node_in_mask "$nid" has_memory; then
		pn_unbind_kmem_private
		return 1
	fi
	if [ "$driver" != kmem ]; then
		if ! echo "$name" > /sys/bus/dax/drivers/kmem/new_id 2>/dev/null; then
			pn_unbind_kmem_private
			return 1
		fi
		PN_NEW_ID=1
	fi
	if ! echo unplugged > "$dev/state" 2>/dev/null ||
	   ! echo 1 > "$dev/private" 2>/dev/null ||
	   ! echo online_movable > "$dev/state" 2>/dev/null ||
	   ! pn_node_is_private "$nid"; then
		pn_unbind_kmem_private
		return 1
	fi
}

# Restore the device recorded by pn_bind_kmem_private(), if any.
pn_unbind_kmem_private()
{
	local dev=${PN_DEV:-} name

	[ -n "$dev" ] || return 0
	PN_DEV=
	name=$(basename "$dev")
	if [ "$(basename "$(readlink "$dev/driver" 2>/dev/null)")" = kmem ]; then
		echo unplugged > "$dev/state" 2>/dev/null || return 1
		[ -z "$PN_ORIG_PRIVATE" ] ||
			echo "$PN_ORIG_PRIVATE" > "$dev/private" 2>/dev/null
	fi
	if [ "$PN_ORIG_DRIVER" = kmem ]; then
		case "$PN_ORIG_STATE" in
		unplugged|offline) ;;
		*) echo "$PN_ORIG_STATE" > "$dev/state" 2>/dev/null ;;
		esac
		return 0
	fi
	[ -e "$dev/driver" ] &&
		echo "$name" > /sys/bus/dax/drivers/kmem/unbind 2>/dev/null
	[ -z "$PN_NEW_ID" ] ||
		echo "$name" > /sys/bus/dax/drivers/kmem/remove_id 2>/dev/null
	[ -z "$PN_ORIG_DRIVER" ] ||
		echo "$name" > "/sys/bus/dax/drivers/$PN_ORIG_DRIVER/bind" 2>/dev/null
	return 0
}

# Enable the cpuset controller below the root, and remember whether it
# was already enabled for pn_disable_cpuset().
pn_enable_cpuset()
{
	grep -qw cpuset "$CGROOT/cgroup.subtree_control" && return 0
	echo +cpuset > "$CGROOT/cgroup.subtree_control" 2>/dev/null || return 1
	PN_CPUSET_ENABLED=1
}

pn_disable_cpuset()
{
	[ -n "${PN_CPUSET_ENABLED:-}" ] || return 0
	PN_CPUSET_ENABLED=
	echo -cpuset > "$CGROOT/cgroup.subtree_control" 2>/dev/null
}

pn_require_root()
{
	[ "$(id -u)" = 0 ] || {
		ktap_skip_all "must be run as root"
		exit "$KSFT_SKIP"
	}
}

# An explicitly selected device must provision; auto may find none.
pn_skip_or_fail()
{
	if [ "$DAX_KMEM_TEST_DEV" = auto ]; then
		ktap_skip_all "$1"
		exit "$KSFT_SKIP"
	fi
	ktap_exit_fail_msg "$1"
}

# The helper tools pass node masks as a single unsigned long.
pn_require_mask_nodes()
{
	local nid

	for nid in "$@"; do
		[ "$nid" -lt "$(getconf LONG_BIT)" ] || {
			ktap_skip_all "node $nid does not fit the helper node mask"
			exit "$KSFT_SKIP"
		}
	done
}

pn_require_test_device()
{
	[ -n "${DAX_KMEM_TEST_DEV:-}" ] || {
		ktap_skip_all "set DAX_KMEM_TEST_DEV=<daxX.Y|auto> to select a test device"
		exit "$KSFT_SKIP"
	}
}
