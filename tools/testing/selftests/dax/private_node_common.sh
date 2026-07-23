# SPDX-License-Identifier: GPL-2.0
#
# Shared provisioning and helpers for the private-node selftests.
# Source after ktap_helpers.sh.
#
# A private node is a NUMA node hotplugged as private (N_MEMORY, not N_MEMORY_FALLBACK).
#
# Enter private mode by:
#   - binding a dax device to kmem
#   - set "state=unplugged"
#   - set "mm_capabilities" to a value other than NODE_MEMORY_CAP_ALL (~0);
#     the set bits are the services (reclaim/user_numa/demotion/...) it opts into
#   - set "state=online"
#
# mm_capabilities == NODE_MEMORY_CAP_ALL brings the memory up as a normal
# N_MEMORY node instead.  dax_file and adistance are separate attributes.
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

# NODE_MEMORY_CAP_* bit values -- keep in sync with include/linux/nodemask.h.
# A device's mm_capabilities is this mask: NODE_MEMORY_CAP_ALL (~0) brings the
# memory up as a normal N_MEMORY node; any other value is a private node that
# opts only into the set bits.
PN_CAP_ALL=0xffffffffffffffff
pn__cap_bit() {
	case "$1" in
	reclaim) echo 1;;    demotion) echo 2;;   numa_balancing) echo 4;;
	ltpin) echo 8;;      damon) echo 16;;     ksm) echo 32;;
	collapse) echo 64;;  hugetlb) echo 128;;  user_numa) echo 256;;
	fallback) echo 512;;
	*) echo 0;;
	esac
}
# pn_dev_setcap DEV CAP 0|1 -- set/clear a capability bit in DEV's
# mm_capabilities (read-modify-write); a normal (ALL) device becomes private.
pn_dev_setcap() {
	local f="$DAX_BASE/$1/mm_capabilities" bit cur
	bit=$(pn__cap_bit "$2")
	cur=$(( $(cat "$f" 2>/dev/null) ))
	[ "$cur" -eq -1 ] 2>/dev/null && cur=0	# was ALL(normal) -> private base
	if [ "$3" = 1 ]; then cur=$((cur | bit)); else cur=$((cur & ~bit)); fi
	printf '0x%x\n' "$cur" > "$f" 2>/dev/null
}
# pn_dev_getcap DEV CAP -- echo 1 if the capability bit is set, else 0.
pn_dev_getcap() {
	local bit; bit=$(pn__cap_bit "$2")
	[ $(( $(cat "$DAX_BASE/$1/mm_capabilities" 2>/dev/null) & bit )) -ne 0 ] &&
		echo 1 || echo 0
}
# pn_dev_is_private DEV -- true if DEV is private, i.e. the FALLBACK bit is clear
# (not in the page-allocator fallback set).
pn_dev_is_private() {
	[ "$(pn_dev_getcap "$1" fallback)" = 0 ]
}

# pn__make_private DIR -- take a freshly kmem-bound device to a private baseline:
# unplugged, private (mm_capabilities = 0, no caps), dax_file mmap available.
pn__make_private() {
	local d=$1
	echo unplugged > "$d/state" 2>/dev/null
	echo 0 > "$d/mm_capabilities" 2>/dev/null
	echo 1 > "$d/dax_file" 2>/dev/null
}

pn__find_bound() {	# echo a dax device already in kmem private mode, if any
	local d drv
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/mm_capabilities" ] || continue
		drv=$(readlink "$d/driver" 2>/dev/null)
		[ "$(basename "${drv:-}")" = kmem ] || continue
		pn_dev_is_private "$(basename "$d")" &&
			{ basename "$d"; return 0; }
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
	{ [ -e "$D/state" ] && [ -e "$D/mm_capabilities" ]; } ||
		{ ktap_skip_all "$DAX missing mm_capabilities attribute"; exit "$KSFT_SKIP"; }
}

# pn_set ATTR VAL -- capability names route through mm_capabilities (private caps),
# any other attribute (dax_file, adistance, state) is written directly.
pn_set() {
	case "$1" in
	reclaim|demotion|numa_balancing|ltpin|damon|ksm|collapse|hugetlb|user_numa)
		pn_dev_setcap "$DAX" "$1" "$2" ;;
	private)	# base config: 1 => private (no caps), 0 => normal (all caps)
		[ "$2" = 1 ] && echo 0 > "$D/mm_capabilities" 2>/dev/null \
			      || echo "$PN_CAP_ALL" > "$D/mm_capabilities" 2>/dev/null ;;
	*)	echo "$2" > "$D/$1" 2>/dev/null ;;
	esac
}
# pn_get ATTR -- capability names read back from mm_capabilities (0/1), others direct.
pn_get() {
	case "$1" in
	reclaim|demotion|numa_balancing|ltpin|damon|ksm|collapse|hugetlb|user_numa)
		pn_dev_getcap "$DAX" "$1" ;;
	*)	cat "$D/$1" 2>/dev/null ;;
	esac
}
pn_hotplug() { echo "$1" > "$D/state" 2>/dev/null; }	# pn_hotplug STATE (rc=status)
pn_state()   { cat "$D/state" 2>/dev/null; }
pn_is_private() { pn_dev_is_private "$DAX"; }	# device opt-in config (persists across offline)
# There is no N_MEMORY_PRIVATE node state; a private node is an N_MEMORY node
# without the FALLBACK cap.  A node onlined with mm_capabilities != ALL is a live
# private node: onlineness shows in has_memory and its caps in nodeN/mem_features
# (see pn_node_is_public); unplug is observed as the node leaving has_memory.
pn_node_online()      { node_in_mask "$PN" has_memory; }		# PN has memory online
pn_online_private()   { pn_node_online && pn_is_private; }	# online AND private-configured
# pn_online_private_at NODE DAX -- true if NODE is online (has memory) and its
# backing DAX device is configured private, i.e. it onlined as a private node.
# For tests that juggle several private nodes (their own $NODE/$DAX pairs).
pn_online_private_at() {
	node_in_mask "$1" has_memory && pn_dev_is_private "$2"
}

# pn_reset -- best-effort return to an unplugged, all-caps-cleared baseline.
pn_reset() {
	pn_hotplug unplugged 2>/dev/null
	[ -e "$D/mm_capabilities" ] || return 0
	pn_dev_is_private "$DAX" && echo 0 > "$D/mm_capabilities" 2>/dev/null
	return 0
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
# NODE_MEMORY_CAP_FALLBACK = 1<<9 (public node bit in nodeN/mem_features)
PN_CAP_FALLBACK=$((1 << 9))

# Is node $1 public (a DRAM/fallback node)?  True iff its mem_features bitmask
# has the FALLBACK cap bit.  Under the additive model has_memory includes private
# nodes, so mem_features is the discriminator (no has_private/has_fallback mask).
pn_node_is_public() {
	local mf
	mf=$(cat "$NODE_BASE/node$1/mem_features" 2>/dev/null) || return 1
	[ $(( ${mf:-0} & PN_CAP_FALLBACK )) -ne 0 ]
}

pn_dram_list() {
	local tok lo hi n out=
	for tok in $(tr ',' ' ' < "$NODE_BASE/has_memory"); do
		lo=${tok%-*}; hi=${tok#*-}
		for n in $(seq "$lo" "$hi"); do
			pn_node_is_public "$n" && out="$out $n"
		done
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
