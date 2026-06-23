# SPDX-License-Identifier: GPL-2.0
#
# Shared provisioning and helpers for the private-node selftests.
# Source after ktap_helpers.sh.
#
# A private node is NUMA node hotplugged as N_MEMORY_PRIVATE.
#
# Enter private mode by:
#   - binding a dax device to kmem
#   - set "state=unplugged"
#   - set "private=1"
#   - set "state=online"
#
# Setting private mode exposes additional sysfs entries:
#   - opt-in attrs (reclaim/user_numa/hotunplug/demotion/numa_balancing/ltpin)
#   - dax/adistance
#
# Additionally, daxN.N may mmap fault directly from the registered node.
#
# Provisioning needs a pre-registered DAX device, which can be emulated
# via kernel boot param - e.g.:    memmap=1G!4G
#
# the devdax namespace is then created automatically by pn_provision().
#
# SKIPs if no suitable device is found.

DAX_BASE=/sys/bus/dax/devices
NODE_BASE=/sys/devices/system/node

pn_require_root() {
	[ "$(id -u)" = 0 ] || { ktap_skip_all "must be run as root"; exit "$KSFT_SKIP"; }
}

TOOL="$DIR/private_node_tool"

pn_begin() {
	ktap_print_header
	pn_require_root
}

# pn_require_tool -- SKIP the whole test if the userspace helper isn't built.
pn_require_tool() {
	[ -x "$TOOL" ] || { ktap_skip_all "private_node_tool not built"; exit "$KSFT_SKIP"; }
}

# pn_need_gup_test -- ensure the gup_test debugfs ioctl is available, else SKIP.
pn_need_gup_test() {
	grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
	[ -e /sys/kernel/debug/gup_test ] ||
		{ ktap_skip_all "gup_test unavailable (need CONFIG_GUP_TEST=y)"; exit "$KSFT_SKIP"; }
}

# pn_field KEY -- read stdin, echo the value after "KEY=", e.g.
# pn_field "on_node$PN" <<<"$out".
pn_field() {
	sed -n "s/.*$1=\([0-9a-z]*\).*/\1/p" | head -1
}

# nodelist_has LIST NID -- true if NID is set in a nodelist string like "0,2-3".
nodelist_has() {
	local nid=$2 tok lo hi
	for tok in $(echo "$1" | tr ',' ' '); do
		lo=${tok%-*}; hi=${tok#*-}
		[ "$nid" -ge "$lo" ] 2>/dev/null && [ "$nid" -le "$hi" ] 2>/dev/null && return 0
	done
	return 1
}

# node_in_mask NID MASKFILE -- true if NID is set in $NODE_BASE/MASKFILE.
node_in_mask() {
	[ -r "$NODE_BASE/$2" ] && nodelist_has "$(cat "$NODE_BASE/$2")" "$1"
}

# pn__make_private DIR -- take a freshly kmem-bound device and:
#   - unplugged
#   - private=1,
#   - dax_file=1 (/dev/daxN.N anon-fault mmap is available)
#   - all per-service caps cleared.
pn__make_private() {
	local d=$1 c
	echo unplugged > "$d/state" 2>/dev/null
	echo 1 > "$d/private" 2>/dev/null
	echo 1 > "$d/dax_file" 2>/dev/null
	for c in reclaim user_numa hotunplug demotion numa_balancing ltpin; do
		[ -e "$d/$c" ] && echo 0 > "$d/$c" 2>/dev/null
	done
}

pn__find_bound() {	# echo a dax device already in kmem private mode, if any
	local d drv
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/private" ] || continue
		drv=$(readlink "$d/driver" 2>/dev/null)
		[ "$(basename "${drv:-}")" = kmem ] || continue
		[ "$(cat "$d/private" 2>/dev/null)" = 1 ] && { basename "$d"; return 0; }
	done
	return 1
}

pn__bind_one() {	# bind the first device_dax dax device on a memoryless node
	local d nid drv
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node")
		[ "$nid" -ge 0 ] 2>/dev/null || continue
		node_in_mask "$nid" has_memory && continue	# need a memoryless node
		drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
		[ "$drv" = device_dax ] &&
			basename "$d" > /sys/bus/dax/drivers/device_dax/unbind 2>/dev/null
		[ "$drv" = kmem ] ||
			basename "$d" > /sys/bus/dax/drivers/kmem/new_id 2>/dev/null
		sleep 1
		pn__make_private "$d"
		return 0
	done
}

# pn_provision -- locate (or create+bind) a kmem-private dax device on a memoryless
# node.  On success sets DAX, D (its sysfs dir) and PN (its target node).
# SKIPs the whole test otherwise.
pn_provision() {
	modprobe -q nd_e820 dax_pmem device_dax nd_pmem 2>/dev/null
	modprobe -q kmem 2>/dev/null
	[ -d /sys/bus/dax/drivers/kmem ] ||
		{ ktap_skip_all "kmem driver unavailable (CONFIG_DEV_DAX_KMEM)"; exit "$KSFT_SKIP"; }

	DAX=$(pn__find_bound)
	if [ -z "$DAX" ]; then
		# devdax-mode namespaces are what materialise bindable dax devices
		if command -v ndctl >/dev/null 2>&1; then
			local r
			for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
				ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f \
					>/dev/null 2>&1
			done
		fi
		pn__bind_one
		DAX=$(pn__find_bound)
	fi
	[ -n "$DAX" ] ||
		{ ktap_skip_all "no kmem-bindable dax device on a memoryless node (see header for memmap= provisioning)"; exit "$KSFT_SKIP"; }

	D=$DAX_BASE/$DAX
	PN=$(cat "$D/target_node" 2>/dev/null)
	{ [ -n "$PN" ] && [ "$PN" -ge 0 ] 2>/dev/null; } ||
		{ ktap_skip_all "$DAX has no valid target_node"; exit "$KSFT_SKIP"; }
	{ [ -e "$D/state" ] && [ -e "$D/reclaim" ]; } ||
		{ ktap_skip_all "$DAX missing private-mode cap attributes"; exit "$KSFT_SKIP"; }
}

pn_set()     { echo "$2" > "$D/$1" 2>/dev/null; }	# pn_set ATTR VAL  (rc=write status)
pn_get()     { cat "$D/$1" 2>/dev/null; }
pn_hotplug() { echo "$1" > "$D/state" 2>/dev/null; }	# pn_hotplug STATE (rc=status)
pn_state()   { cat "$D/state" 2>/dev/null; }
pn_is_private() { node_in_mask "$PN" has_private_memory; }

# pn_reset -- best-effort return to an unplugged, all-caps-cleared baseline.
pn_reset() {
	pn_hotplug unplugged 2>/dev/null
	local c
	for c in ltpin numa_balancing demotion hotunplug user_numa reclaim; do
		[ -e "$D/$c" ] && echo 0 > "$D/$c" 2>/dev/null
	done
}

# pn_provision_all
#
# bind EVERY device_dax device on a memoryless node to kmem private mode,
# this is used for the multi-private-node tests.
#
# On success sets PN_DAXES and PN_NODES (space-separated, index-aligned).
#
# Returns 0 always.
# Caller checks how many nodes were found and SKIPs if too few.
pn_provision_all() {
	modprobe -q nd_e820 dax_pmem device_dax nd_pmem 2>/dev/null
	modprobe -q kmem 2>/dev/null
	if command -v ndctl >/dev/null 2>&1; then
		local r
		for r in $(ndctl list -R 2>/dev/null | grep -oE 'region[0-9]+'); do
			ndctl create-namespace -m devdax -e "${r/region/namespace}.0" -f \
				>/dev/null 2>&1
		done
	fi
	local d nid drv
	PN_DAXES=; PN_NODES=
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] || continue
		nid=$(cat "$d/target_node"); [ "$nid" -ge 0 ] 2>/dev/null || continue
		node_in_mask "$nid" has_memory && continue	# memoryless only
		drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
		[ "$drv" = device_dax ] &&
			basename "$d" > /sys/bus/dax/drivers/device_dax/unbind 2>/dev/null
		[ "$drv" = kmem ] ||
			basename "$d" > /sys/bus/dax/drivers/kmem/new_id 2>/dev/null
		sleep 1
		pn__make_private "$d"
		PN_DAXES="$PN_DAXES $(basename "$d")"; PN_NODES="$PN_NODES $nid"
	done
	PN_DAXES=${PN_DAXES# }; PN_NODES=${PN_NODES# }
	sleep 1
}

# pn_swap_setup -- ensure at least one swap area is active.
# Reuses an existing one, else swaps on the first unmounted block device
# Returns 0 on success
# Caller SKIPs on failure.  NEVER touches a mounted device.
pn_swap_setup() {
	[ "$(grep -c . /proc/swaps)" -gt 1 ] && return 0
	local d
	for d in /dev/vd? /dev/sd? /dev/nvme?n?; do
		[ -b "$d" ] || continue
		grep -q "^$d " /proc/mounts && continue		# in use as a fs
		swapon "$d" 2>/dev/null && return 0
		mkswap "$d" >/dev/null 2>&1 && swapon "$d" 2>/dev/null && return 0
	done
	return 1
}

# Global-toggle save/restore.
#
# Save/REstore system-wide knob (thp, numa_balancing, etc) across tests.
# Snapshot with pn_save_global / pn_snapshot_swaps
# arrange restoration with:
#     trap pn_restore_globals EXIT INT TERM
PN_SAVED=()			# "path=value" knobs to restore
PN_SWAP_SNAP=""			# active swap devices at snapshot time (if taken)

# Record a knob's current value.  For "list" files like THP enabled
# ("always [madvise] never") only the active token is saved.
pn_save_global() {
	local v
	v=$(cat "$1" 2>/dev/null) || return 0
	case "$v" in
	*'['*) v=$(printf '%s' "$v" | sed -n 's/.*\[\([^]]*\)\].*/\1/p') ;;
	esac
	PN_SAVED+=("$1=$v")
}

# Snapshot the active swap set so pn_restore_globals() can re-enable exactly it.
pn_snapshot_swaps() {
	local dev rest
	PN_SWAP_SNAP=" "
	while read -r dev rest; do
		[ "$dev" = Filename ] && continue
		PN_SWAP_SNAP="$PN_SWAP_SNAP$dev "
	done < /proc/swaps
}

pn_restore_globals() {
	local e d
	for e in "${PN_SAVED[@]}"; do
		printf '%s\n' "${e#*=}" > "${e%%=*}" 2>/dev/null
	done
	if [ -n "$PN_SWAP_SNAP" ]; then
		swapoff -a 2>/dev/null
		for d in $PN_SWAP_SNAP; do swapon "$d" 2>/dev/null; done
	fi
}

# pn_dram_list
# print N_MEMORY node ids, space separated, expanded
# from the "0,3" / "0-1" style nodelist in has_memory.
pn_dram_list() {
	local tok lo hi n out=
	for tok in $(tr ',' ' ' < "$NODE_BASE/has_memory"); do
		lo=${tok%-*}; hi=${tok#*-}
		for n in $(seq "$lo" "$hi"); do out="$out $n"; done
	done
	echo "${out# }"
}

# nm_line PID ADDR
# echo the /proc/PID/numa_maps line for the VMA at ADDR
# (ADDR may be 0x-prefixed).  Empty if not found.
nm_line() {
	local a=${2#0x}
	awk -v a="$a" '$1==a {print; exit}' "/proc/$1/numa_maps" 2>/dev/null
}

# nm_policy PID ADDR -- echo the policy token (field 2), e.g. "bind:0,3".
nm_policy() { nm_line "$1" "$2" | awk '{print $2}'; }

# nm_on_node PID ADDR NID -- echo pages of that VMA resident on node NID (0 if none).
nm_on_node() {
	nm_line "$1" "$2" | tr ' ' '\n' |
		awk -F= -v n="N$3" '$1==n {print $2; found=1} END{if(!found) print 0}'
}

# nm_nodes PID ADDR -- echo the "N<nid>=<pages>" residency tokens.
nm_nodes() { nm_line "$1" "$2" | tr ' ' '\n' | grep '^N[0-9]'; }

# pn_cgroup2 -- echo a cgroup2 mount root with cpuset delegated to subtree
# control, or return 1 if cgroup2/cpuset is unavailable.
pn_cgroup2() {
	local root
	root=$(awk '$3=="cgroup2"{print $2; exit}' /proc/mounts)
	if [ -z "$root" ]; then
		root=/sys/fs/cgroup
		mkdir -p "$root" 2>/dev/null
		mount -t cgroup2 none "$root" 2>/dev/null
		root=$(awk '$3=="cgroup2"{print $2; exit}' /proc/mounts)
	fi
	[ -n "$root" ] || return 1
	grep -qw cpuset "$root/cgroup.controllers" 2>/dev/null || return 1
	grep -qw cpuset "$root/cgroup.subtree_control" 2>/dev/null ||
		echo "+cpuset" > "$root/cgroup.subtree_control" 2>/dev/null
	echo "$root"
}
