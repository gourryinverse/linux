# SPDX-License-Identifier: GPL-2.0
#
# Shared provisioning and helpers for the private-node selftests.
# Source after ktap_helpers.sh.
#
# A private node is a NUMA node whose memory is kept out of the page
# allocator's fallback zonelists, so only an explicit bind reaches it.
#
# Features are declared when the node is provisioned, not written at
# runtime.  Provision one of two ways:
#
#   private_node=<nid>[,<features>]                 on the kernel command line
#   modprobe dax_test features=<mask>[,<mask>...] \
#            target_node=<nid> range_start=<pa> \
#            range_size=<len> [adistance=N]         a dax provider
#
# and place memory on it by mmap()ing $PN_ANON, which hands back anonymous
# memory bound to the selected node -- the only route onto a node that did not
# opt into userspace placement.
#
# SKIPs if no private node is present.

DAX_BASE=/sys/bus/dax/devices
NODE_BASE=/sys/devices/system/node
DAX_TEST_DEBUGFS=/sys/kernel/debug/dax_test
# mmap()ing this yields anonymous memory on whichever node pn_select() chose,
# which is how a test places memory on a node no mempolicy may name.  It
# replaces the dax-file cdev the kmem driver used to grow for this purpose.
PN_ANON=$DAX_TEST_DEBUGFS/anon

# Features are fixed when a node is provisioned -- there is no runtime knob.
# A private node comes from either:
#
#   private_node=<nid>[,<features>]     on the kernel command line, or
#   modprobe dax_test features=<mask> target_node=.. \
#            range_start=.. range_size=..
#
# so a test that needs two feature classes asks for two nodes rather than
# toggling one.  Nodes are classified from the kernel's own nodelists.

# pn_nodelist_has FILE NID -- membership in a "0,2-3" nodelist; true if absent
pn_nodelist_has() {
	local f=$1 nid=$2 tok lo hi

	[ -r "$f" ] || return 0
	for tok in $(tr ',' ' ' < "$f"); do
		lo=${tok%-*}; hi=${tok#*-}
		[ "$nid" -ge "$lo" ] 2>/dev/null && [ "$nid" -le "$hi" ] 2>/dev/null &&
			return 0
	done
	return 1
}

# A private node has memory but is not on the allocator fallback lists.  Both
# halves matter: a memoryless node is in neither list, and testing only the
# fallback list would report every idle dax target as private.
pn_node_is_private() {
	pn_nodelist_has "$NODE_BASE/has_memory" "$1" &&
		! pn_nodelist_has "$NODE_BASE/has_public_memory" "$1"
}

# pn__dev_is_provider DEV -- true if DEV came from the dax test provider, which
# is the only one that declares a feature mask.  A device's mask is not visible
# in sysfs, so its provider is what tells us it can bring a node up private.
pn__dev_is_provider() {
	case "$(readlink -f "$DAX_BASE/$1" 2>/dev/null)" in
	*/dax_test.*)	return 0 ;;
	esac
	return 1
}

# True if userspace may place memory on the node at all.
pn_node_is_usernuma() {
	pn_nodelist_has "$NODE_BASE/has_user_memory" "$1"
}

# pn_node_features NID -- the node's NODE_MEMORY_FEAT_* mask.
#
# The kernel does not publish the raw mask; the bit layout is internal and
# would otherwise become ABI.  Derive it from whoever provisioned the node
# instead.  A mask the kernel would not honour is refused rather than adjusted,
# so a node that came up private carries exactly what was asked for.
pn_node_features() {
	local nid=$1 tok mask tn

	# public: every feature, by definition
	pn_nodelist_has "$NODE_BASE/has_public_memory" "$nid" &&
		{ echo $(( PN_FEAT_ALL )); return; }

	# boot-provisioned: private_node=<nid>[,<mask>], mask defaults to none
	for tok in $(tr ' ' '\n' < /proc/cmdline | grep '^private_node='); do
		tok=${tok#private_node=}
		[ "${tok%%,*}" = "$nid" ] || continue
		case "$tok" in *,*) mask=${tok#*,} ;; *) mask=0 ;; esac
		echo $(( mask )); return
	done

	# provider-provisioned: the test provider declares its mask up front
	tn=$(cat /sys/module/dax_test/parameters/target_node 2>/dev/null)
	if [ "${tn:--1}" = "$nid" ]; then
		mask=$(tr -d ' ' < /sys/module/dax_test/parameters/features \
			2>/dev/null | cut -d, -f1)
		[ -n "$mask" ] && { echo $(( mask )); return; }
	fi

	# provisioned by something we do not know: report only what sysfs states,
	# so a test asking for an inner feature skips rather than mis-asserts
	pn_nodelist_has "$NODE_BASE/has_user_memory" "$nid" &&
		{ echo $(( $(pn__feat_mask user_numa) )); return; }
	echo 0
}

# pn_node_has_feature NID BIT -- test one feature bit of the raw mask
pn_node_has_feature() {
	local features; features=$(pn_node_features "$1")
	[ -n "$features" ] && [ $(( features & $2 )) -ne 0 ]
}

# pn_anon_bind NID -- point the test driver's anon mapping at NID.
# mmap()ing $DAX_TEST_DEBUGFS/anon then yields anonymous memory on that node,
# which is the only way to place on a node without USER_NUMA.
pn_anon_bind() {
	[ -w "$DAX_TEST_DEBUGFS/bind_node" ] || return 1
	echo "$1" > "$DAX_TEST_DEBUGFS/bind_node" 2>/dev/null
}

# pn_provider_load -- bring dax_test up as a provider over PN_DAX_TEST_RANGE.
#
# The nd/pmem providers never declare a feature mask, so every dax device they
# create is NODE_MEMORY_FEAT_ALL, i.e. public.  dax_test is the only provider
# that declares one, so a device-backed private node has to come from it.  It
# needs a range of its own: the nd stack claims each pmem region with a
# namespace, so free the one covering our range first.
pn_provider_load() {
	local r ns

	[ -n "${PN_DAX_TEST_RANGE_START:-}" ] || return 0

	modprobe -q nd_e820 dax_pmem nd_pmem device_dax kmem 2>/dev/null
	if command -v ndctl >/dev/null 2>&1; then
		for r in /sys/bus/nd/devices/region[0-9]*; do
			[ -r "$r/resource" ] || continue
			ns="$(basename "$r" | sed 's/region/namespace/').0"
			if [ "$(( $(cat "$r/resource") ))" = \
			     "$(( PN_DAX_TEST_RANGE_START ))" ]; then
				# Leave this range free for dax_test to claim.
				ndctl disable-namespace "$ns" >/dev/null 2>&1
				ndctl destroy-namespace "$ns" -f >/dev/null 2>&1
			else
				ndctl create-namespace -m devdax -e "$ns" -f \
					>/dev/null 2>&1
			fi
		done
	fi

	modprobe -q dax_test \
		"target_node=${PN_DAX_TEST_NODE}" \
		"range_start=${PN_DAX_TEST_RANGE_START}" \
		"range_size=${PN_DAX_TEST_RANGE_SIZE}" \
		"features=${PN_DAX_TEST_FEATURES:-0x1ff}" \
		"adistance=${PN_DAX_TEST_ADISTANCE:-2880}" \
		"adist_node=${PN_DAX_TEST_NODE}" 2>/dev/null
}

# pn_need_dax_test -- SKIP unless the test provider is loaded.
pn_need_dax_test() {
	[ -d "$DAX_TEST_DEBUGFS" ] || pn_provider_load
	modprobe -q dax_test 2>/dev/null
	[ -d "$DAX_TEST_DEBUGFS" ] ||
		{ ktap_skip_all "dax_test not available (need CONFIG_DEV_DAX_TEST)"; exit "$KSFT_SKIP"; }
}

# pn_find_node CLASS -- first node of a class: "private", "usernuma"
# (private + USER_NUMA), "plain" (private, no USER_NUMA) or "public".
pn_find_node() {
	local n nid
	for n in $NODE_BASE/node[0-9]*; do
		nid=${n##*node}
		[ -r "$n/meminfo" ] || continue
		case "$1" in
		private)  pn_node_is_private "$nid" || continue ;;
		usernuma) pn_node_is_private "$nid" && pn_node_is_usernuma "$nid" || continue ;;
		plain)    pn_node_is_private "$nid" && ! pn_node_is_usernuma "$nid" || continue ;;
		public)   pn_node_is_private "$nid" && continue ;;
		esac
		echo "$nid"; return 0
	done
	return 1
}

NODE_BASE=/sys/devices/system/node

# pn_need_debugfs -- mount debugfs if needed; the test provider lives there.
pn_need_debugfs() {
	grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
}

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
	pn_need_debugfs
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

# NODE_MEMORY_FEAT_* bit values -- keep in sync with include/linux/nodemask.h.
# A provider declares this mask for its memory: NODE_MEMORY_FEAT_ALL (~0) brings
# it up as an ordinary N_MEMORY node; any other value makes it a private node
# opting only into the bits it sets.
# pn_dev_is_private DEV -- true if DEV's target node is private.
pn_dev_is_private() {
	local nid
	nid=$(cat "$DAX_BASE/$1/target_node" 2>/dev/null) || return 1
	pn_node_is_private "$nid"
}

# pn__make_private DIR -- a device's features are declared by its provider,
# so all that is left here is to park it unplugged.
pn__make_private() {
	echo unplugged > "$1/state" 2>/dev/null
}

pn__find_bound() {	# echo a dax device already in kmem private mode, if any
	local d drv
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/adistance" ] || continue	# kmem-bound marker
		drv=$(readlink "$d/driver" 2>/dev/null)
		[ "$(basename "${drv:-}")" = kmem ] || continue
		{ pn_dev_is_private "$(basename "$d")" ||
		  pn__dev_is_provider "$(basename "$d")"; } &&
			{ basename "$d"; return 0; }
	done
	return 1
}

pn__bind_one() {	# bind every device_dax dax device on a memoryless node
	local d nid drv bound=1 pass
	for pass in provider other; do
	for d in "$DAX_BASE"/dax*; do
		if [ "$pass" = provider ]; then
			pn__dev_is_provider "$(basename "$d")" || continue
		else
			pn__dev_is_provider "$(basename "$d")" && continue
		fi
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
		bound=0
	done
	done
	return $bound
}

# pn_provision -- locate (or create+bind) a kmem-private dax device on a memoryless
# node.  On success sets DAX, D (its sysfs dir) and PN (its target node).
# SKIPs the whole test otherwise.
pn_provision() {
	local nid

	pn_need_debugfs
	pn_need_dax_test	# supplies the node-bound anon mapping

	# A private node may come from the command line (private_node=) or from
	# a provider; either way it is already configured by the time we look.
	nid=$(pn_find_node private)
	if [ -z "$nid" ]; then
		# no boot-provisioned node: fall back to binding a dax device
		pn_provider_load
		if [ -z "${PN_DAX_TEST_RANGE_START:-}" ]; then
			modprobe -q nd_e820 dax_pmem device_dax nd_pmem kmem 2>/dev/null
			if command -v ndctl >/dev/null 2>&1; then
				local r
				for r in $(ndctl list -R 2>/dev/null |
					   grep -oE 'region[0-9]+'); do
					ndctl create-namespace -m devdax \
						-e "${r/region/namespace}.0" -f \
						>/dev/null 2>&1
				done
			fi
		fi
		pn__bind_one
		nid=$(pn_find_node private)
	fi
	[ -n "$nid" ] ||
		{ ktap_skip_all "no private memory node online (boot with private_node=<nid>[,<features>] or load dax_test)"; exit "$KSFT_SKIP"; }

	pn_select "$nid"

	# A backing dax device is optional; device-lifecycle tests need one.
	DAX=$(pn__find_bound 2>/dev/null)
	[ -n "$DAX" ] && D=$DAX_BASE/$DAX || { DAX=; D=; }
}

# pn_require_dax -- the device-lifecycle tests need a node they can plug and
# unplug.  pn_provision() may have settled on a boot-provisioned node, which has
# no device, so look for a dax-backed one before giving up.
pn_require_dax() {
	local d nid

	PN_NEED_DAX=1

	# pn_provision() prefers a boot-provisioned node and leaves dax devices
	# unbound; bind one now so there is something to plug and unplug.
	if ! pn__dax_for_node "$PN" >/dev/null 2>&1; then
		pn_provider_load
		if [ -z "${PN_DAX_TEST_RANGE_START:-}" ]; then
			modprobe -q nd_e820 dax_pmem device_dax nd_pmem kmem 2>/dev/null
			if command -v ndctl >/dev/null 2>&1; then
				for d in $(ndctl list -R 2>/dev/null |
					   grep -oE 'region[0-9]+'); do
					ndctl create-namespace -m devdax \
						-e "${d/region/namespace}.0" -f \
						>/dev/null 2>&1
				done
			fi
		fi
		pn__bind_one
	fi

	if [ -z "$D" ] || [ ! -e "$D/state" ] || ! pn_node_is_private "$PN"; then
		local want
		for want in provider any; do
			for d in "$DAX_BASE"/dax*; do
				[ -e "$d/target_node" ] && [ -e "$d/state" ] || continue
				nid=$(cat "$d/target_node" 2>/dev/null)
				[ "$nid" -ge 0 ] 2>/dev/null || continue
				[ "$want" = provider ] &&
					! pn__dev_is_provider "$(basename "$d")" &&
					continue
				DAX=$(basename "$d"); D=$d; PN=$nid
				pn_anon_bind "$PN" 2>/dev/null
				return 0
			done
		done
		ktap_skip_all "needs a dax-backed private node (none bound; carve one with memmap=)"
		exit "$KSFT_SKIP"
	fi
}

# Feature bit values, matching include/linux/nodemask.h.
# Every defined bit; what a public node reports.
PN_FEAT_ALL=511

pn__feat_mask() {
	case "$1" in
	reclaim) echo 1;;    demotion) echo 2;;   numa_balancing) echo 4;;
	ltpin) echo 8;;      damon) echo 16;;     ksm) echo 32;;
	collapse) echo 64;;  user_numa) echo 128;;
	public) echo 256;; *) echo 0;;
	esac
}

# Accumulated requirements on the node under test.  Each pn_set adds one, and
# the node is re-selected against all of them, so a test asking for several
# features gets one node satisfying the lot rather than the last one asked
# for.  pn_reset clears them.
PN_WANT=0
PN_NOT=0
PN_NEED_DAX=0	# set by pn_require_dax: only device-backed nodes will do

# pn__dax_for_node NID -- the dax device whose target_node is NID, if any
pn__dax_for_node() {
	local d
	for d in "$DAX_BASE"/dax*; do
		[ -e "$d/target_node" ] && [ -e "$d/state" ] || continue
		[ "$(cat "$d/target_node" 2>/dev/null)" = "$1" ] && { basename "$d"; return 0; }
	done
	return 1
}

pn__reselect() {
	local nid features dev
	for nid in $(pn_private_nodes); do
		features=$(pn_node_features "$nid")
		[ -n "$features" ] || continue
		if [ "$PN_NEED_DAX" = 1 ]; then
			dev=$(pn__dax_for_node "$nid") || continue
		fi
		[ $(( features & PN_WANT )) -eq $(( PN_WANT )) ] || continue
		[ $(( features & PN_NOT )) -eq 0 ] || continue
		pn_select "$nid"
		if [ "$PN_NEED_DAX" = 1 ]; then
			DAX=$dev; D=$DAX_BASE/$dev
		fi
		return 0
	done
	return 1
}

# pn_set FEATURE 0|1 -- require that the node under test does (1) or does
# not (0) have FEATURE, and re-select accordingly.
#
# Features are fixed when a node is provisioned, so a test that used to
# toggle one node now asks for a node of each class; it skips if the running
# topology has none.  Non-feature attributes (state) still go to the device.
pn_set() {
	local bit

	case "$1" in
	reclaim|demotion|numa_balancing|ltpin|damon|ksm|collapse|hugetlb|user_numa)
		bit=$(pn__feat_mask "$1")
		if [ "$2" = 1 ]; then
			PN_WANT=$(( PN_WANT | bit )); PN_NOT=$(( PN_NOT & ~bit ))
		else
			PN_NOT=$(( PN_NOT | bit )); PN_WANT=$(( PN_WANT & ~bit ))
		fi
		pn__reselect && return 0
		ktap_skip_all "no private node with $1=$2 (add private_node=<nid>,<features> or dax_test features=)"
		exit "$KSFT_SKIP" ;;
	private)
		if [ "$2" = 1 ]; then
			pn__reselect && return 0
			ktap_skip_all "no private node online"; exit "$KSFT_SKIP"
		fi
		PN=$(pn_find_node public) || { ktap_skip_all "no public node online"; exit "$KSFT_SKIP"; }
		return 0 ;;
	*)	echo "$2" > "$D/$1" 2>/dev/null ;;
	esac
}

# pn_get FEATURE -- 1 if the node under test has FEATURE, else 0.
pn_get() {
	case "$1" in
	reclaim|demotion|numa_balancing|ltpin|damon|ksm|collapse|hugetlb|user_numa)
		pn_node_has_feature "$PN" "$(pn__feat_mask "$1")" && echo 1 || echo 0 ;;
	*)	cat "$D/$1" 2>/dev/null ;;
	esac
}

# pn_private_nodes -- ids of every online private node, ascending.
pn_private_nodes() {
	local n nid
	for n in $NODE_BASE/node[0-9]*; do
		nid=${n##*node}
		[ -r "$n/meminfo" ] || continue
		pn_node_is_private "$nid" && echo "$nid"
	done
}

# pn_select NID -- make NID the node under test, and point the test driver's
# anon mapping at it so memory can be placed there.
pn_select() {
	PN=$1
	pn_anon_bind "$PN" 2>/dev/null
}

# pn_online_as STATE -- have the node under test online in STATE and private.
#
# A dax-backed node is driven through its state machine.  A boot-provisioned
# node is already online in a kernel zone and cannot be re-zoned, so a request
# for ZONE_MOVABLE fails and the caller skips: only a device can supply that.
pn_online_as() {
	if [ -n "$D" ]; then
		pn_hotplug "$1"
		[ "$(pn_state)" = "$1" ] && pn_is_private
		return
	fi
	pn_is_private || return 1
	case "$1" in
	online|online_kernel) return 0 ;;
	*)                    return 1 ;;
	esac
}

pn_hotplug() { [ -n "$D" ] || return 0; echo "$1" > "$D/state" 2>/dev/null; }
pn_state()   { [ -n "$D" ] && cat "$D/state" 2>/dev/null; }
pn_is_private() { pn_node_is_private "$PN"; }	# the node under test is private
# There is no N_MEMORY_PRIVATE node state: a private node is an N_MEMORY node
# absent from has_public_memory.  Onlineness shows in has_memory, and unplug
# is observed as the node leaving it.
pn_node_online()      { node_in_mask "$PN" has_memory; }		# PN has memory online
pn_online_private()   { pn_node_online && pn_is_private; }	# online AND private-configured
# pn_online_private_at NODE DAX -- true if NODE is online (has memory) and its
# backing DAX device is configured private, i.e. it onlined as a private node.
# For tests that juggle several private nodes (their own $NODE/$DAX pairs).
pn_online_private_at() {
	node_in_mask "$1" has_memory && pn_dev_is_private "$2"
}

# pn_reset -- drop accumulated feature requirements and park any device.
pn_reset() {
	PN_WANT=0
	PN_NOT=0	# PN_NEED_DAX persists: it is a property of the test
	[ -n "$D" ] && pn_hotplug unplugged 2>/dev/null
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
	pn_need_debugfs	# the feature knob lives there
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
# Is node $1 public (a DRAM/fallback node)?  Under the additive model has_memory
# includes private nodes, so the discriminator is has_public_memory: the nodes
# an allocation reaches without naming them.
pn_node_is_public() {
	nodelist_has "$(cat "$NODE_BASE/has_public_memory" 2>/dev/null)" "$1"
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
