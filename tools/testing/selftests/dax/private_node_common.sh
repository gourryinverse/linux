# SPDX-License-Identifier: GPL-2.0
# Shared helpers used by the CRAM private-node selftests.

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
	node_in_mask "$1" has_memory && ! node_in_mask "$1" has_common_memory
}

pn_node_vmstat()
{
	awk -v key="$2" '$1 == key { print $2; found = 1 }
		END { if (!found) print "" }' "$NODE_BASE/node$1/vmstat" 2>/dev/null
}

pn_node_present_pages()
{
	local kb pagesize

	kb=$(awk '$3 == "MemTotal:" { print $4 }' \
		"$NODE_BASE/node$1/meminfo" 2>/dev/null)
	pagesize=$(getconf PAGESIZE)
	[ -n "$kb" ] && echo $((kb * 1024 / pagesize))
}

# Bind a dax device to the explicit CRAM test provider and leave it offline.
pn_bind_cramdax()
{
	local dev=$1 name driver nid

	name=$(basename "$dev")
	nid=$(cat "$dev/target_node" 2>/dev/null) || return 1
	driver=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null)
	if [ "$driver" = cramdax ]; then
		echo offline > "$dev/state" 2>/dev/null || return 1
		return 0
	fi

	if [ "$driver" = kmem ]; then
		echo unplugged > "$dev/state" 2>/dev/null || return 1
	fi
	if [ -n "$driver" ]; then
		echo "$name" > "/sys/bus/dax/drivers/$driver/unbind" 2>/dev/null ||
			return 1
	fi
	node_in_mask "$nid" has_memory && return 1
	echo "$name" > /sys/bus/dax/drivers/cramdax/new_id 2>/dev/null ||
		return 1
	driver=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null)
	[ "$driver" = cramdax ]
}

# Bind a memoryless dax device to the explicit shared-allocation provider.
pn_bind_anondax()
{
	local dev=$1 name driver nid

	name=$(basename "$dev")
	nid=$(cat "$dev/target_node" 2>/dev/null) || return 1
	driver=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null)
	[ "$driver" = anondax ] && return 0

	if [ "$driver" = kmem ]; then
		echo unplugged > "$dev/state" 2>/dev/null || return 1
	elif [ "$driver" = cramdax ]; then
		echo offline > "$dev/state" 2>/dev/null || return 1
	fi
	if [ -n "$driver" ]; then
		echo "$name" > "/sys/bus/dax/drivers/$driver/unbind" 2>/dev/null ||
			return 1
	fi
	node_in_mask "$nid" has_memory && return 1
	echo "$name" > /sys/bus/dax/drivers/anondax/new_id 2>/dev/null ||
		return 1
	driver=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null)
	[ "$driver" = anondax ]
}

pn_is_private()
{
	pn_node_is_private "$PN"
}

pn_require_root()
{
	[ "$(id -u)" = 0 ] || {
		ktap_skip_all "must be run as root"
		exit "$KSFT_SKIP"
	}
}

PN_SAVED=()
PN_SWAP_SNAP=""

pn_snapshot_swaps()
{
	local dev rest

	PN_SWAP_SNAP=" "
	while read -r dev rest; do
		[ "$dev" = Filename ] && continue
		PN_SWAP_SNAP="$PN_SWAP_SNAP$dev "
	done < /proc/swaps
}

pn_swap_setup()
{
	local dev

	[ "$(grep -c . /proc/swaps)" -gt 1 ] && return 0
	for dev in /dev/vd? /dev/sd? /dev/nvme?n?; do
		[ -b "$dev" ] || continue
		grep -q "^$dev " /proc/mounts && continue
		swapon "$dev" 2>/dev/null && return 0
		mkswap "$dev" >/dev/null 2>&1 && swapon "$dev" 2>/dev/null &&
			return 0
	done
	return 1
}

pn_restore_globals()
{
	local entry dev

	for entry in "${PN_SAVED[@]}"; do
		printf '%s\n' "${entry#*=}" > "${entry%%=*}" 2>/dev/null
	done
	if [ -n "$PN_SWAP_SNAP" ]; then
		swapoff -a 2>/dev/null
		for dev in $PN_SWAP_SNAP; do
			swapon "$dev" 2>/dev/null
		done
	fi
}
