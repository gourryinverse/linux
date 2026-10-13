.. SPDX-License-Identifier: GPL-2.0

====================
Private memory nodes
====================

A private memory node has online System RAM (``N_MEMORY``), but is absent from
``N_MEMORY_COMMON``. Its capacity is not available to ordinary allocations.
This permits specialized memory, such as HBM, to be exposed for explicit
userspace placement without making it a general allocation fallback.

Private nodes support explicit userspace NUMA placement. Placement requires the
node to be reclaimed and compacted like ordinary memory; other memory
management services remain excluded.

Provisioning with DAX/kmem
==========================

With ``CONFIG_NUMA``, ``CONFIG_DEV_DAX_KMEM`` and ``CONFIG_MEMORY_HOTREMOVE``,
bind a device-dax instance to ``kmem`` and select private mode while its memory
is unplugged::

    echo unplugged > /sys/bus/dax/devices/daxX.Y/state
    echo 1 > /sys/bus/dax/devices/daxX.Y/private
    echo online_movable > /sys/bus/dax/devices/daxX.Y/state

Private mode enables userspace NUMA placement together with reclaim and
compaction. The ``private`` attribute defaults to zero, preserving ordinary kmem
behavior, and is hidden without ``CONFIG_NUMA``. Changing its value requires the
unplugged state; repeating the current value does not. Unbinding kmem discards
the setting.

All System RAM on a node must have the same configuration. To change it,
remove every System RAM block on that node first. Merely offlining blocks is
insufficient: they retain the configuration so they can be onlined again.
Removing the last block releases the configuration even if ZONE_DEVICE memory
keeps the node alive.

Ordinary kernel allocations require common memory, so a system must keep at
least one common-memory node. The kernel does not enforce this; boot memory
normally provides it.

Userspace placement
===================

Bind an anonymous or shmem-backed mapping to a single private node with
``mbind(2)``, ``MPOL_BIND`` and ``MPOL_F_PRIVATE_NODES`` before faulting its
pages. For example::

    buf = mmap(NULL, length, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* mask contains exactly the chosen private node */
    mbind(buf, length, MPOL_BIND | MPOL_F_PRIVATE_NODES, mask, maxnode, 0);
    memset(buf, 0, length);

Like ``MPOL_F_STATIC_NODES`` and ``MPOL_F_RELATIVE_NODES``, the flag selects
how cpuset changes affect the binding: a private binding is never rebound, and
each allocation checks its eligibility instead. ``get_mempolicy(2)`` reports
the flag, and older kernels reject it with ``-EINVAL``.

A private binding is rejected with ``-EINVAL`` if:

- its mask names anything but one private node with userspace NUMA placement,
  including an additional memoryless node;
- it is combined with ``MPOL_F_STATIC_NODES`` or ``MPOL_F_RELATIVE_NODES``;
- it applies to a HugeTLB mapping: private nodes cannot hold HugeTLB pages;
- its node is not in the caller's cpuset, as for ordinary bindings.

A binding must be eligible when it is created. A binding whose node later
becomes ineligible is suspended, not rejected; see "Binding eligibility".

Without the flag, private nodes in a mask are ignored, as nodes outside the
cpuset are, so a mask naming only private nodes is rejected with ``-EINVAL``.
libnuma's ``numa_alloc_onnode()`` and ``numa_tonode_memory()`` do not pass the
flag.

``set_mempolicy(2)`` rejects ``MPOL_F_PRIVATE_NODES``, because a task policy
also steers the task's kernel allocations. Preferred, interleave and
preferred-many policies, tmpfs mount policies, HugeTLB pool placement and
automatic NUMA balancing remain confined to common memory.

``cpuset.mems`` includes private nodes by default, so any task may bind to and
exhaust private capacity. Restrict access with ``cpuset.mems``.

``move_pages(2)`` and ``migrate_pages(2)`` can explicitly target private nodes,
subject to their existing permission checks. Status queries report the node of
each resident page on a node with userspace NUMA placement, and ``-ENOENT``
otherwise, as for ``ZONE_DEVICE`` memory.

Ordinary file page cache is never allocated from private memory; explicit
migration can place existing file pages there. A private binding on a shmem
mapping allocates its page cache from the private node, and reclaim evicts it
like other shmem page cache. guest_memfd page cache is unevictable and not
movable, so a binding places it on a private node only if the node has
non-movable memory (``online`` or ``online_kernel``); see the zone restrictions
below.

Allocation and OOM
==================

Allocations follow the ordinary anonymous-memory and memcg-accounting paths.
The private zonelist contains only the target node's local zones, so it cannot
spill into common memory merely because private capacity is exhausted.

If reclaim on the node cannot satisfy an allocation and it reaches
private-memory OOM, the kernel selects the allocating process rather than
scoring unrelated tasks for this capacity. It first waits for an earlier OOM
victim whose memory has not yet been reaped. Normal OOM killing still applies to
processes sharing the victim's address space and to workloads grouped by
``memory.oom.group``. A configured OOM group can therefore include processes
that do not use private memory. Unconditional panic-on-OOM policy still applies.

The allocating process need not own the binding. A process can allocate through
another address space's private binding, for example by writing through
``/proc/pid/mem``, resolving faults with ``UFFDIO_COPY``, prefetching with
``process_madvise(2)`` or running ``swapoff``. If that allocation reaches
private-memory OOM, the kernel selects that process.

An unkillable or OOM-protected allocating task does not cause another task to
be selected. The kernel reports this once and fails the allocation; a user fault
then waits until memory on the node is freed. While it waits, the fault handler
may still log its generic, ratelimited "VM_FAULT_OOM leaked out" message.

Ordinary OOM handling uses common memory only: private capacity is excluded
from the constrained capacity, and tasks whose cpusets share only private nodes
are not eligible victims for each other. Per-task OOM scores still include
resident private pages, since resident memory is not tracked per node.

Private anonymous pages remain charged to memory cgroups; OOM caused by a cgroup
limit retains the existing memcg OOM behavior.

cpuset interaction
==================

``cpuset.mems`` grants permission, not placement: ordinary allocation still uses
common memory, while private placement requires a private binding or explicit
migration.

Cgroup v2 and ``cpuset_v2_mode`` retain a private-only configured mask. Its
effective mask keeps the requested private nodes allowed by the parent and
adds the parent's common nodes, without inheriting unrequested private nodes.
Legacy v1 has no such fallback: setting a private-only mask or attaching a task
to one fails with ``-ENOSPC``. Hot-unplug of the last common node empties a
legacy cpuset's mask and moves its tasks to an ancestor.
In v2, hotplug changes only the effective mask, not the configured
``cpuset.mems`` request. A literally empty request inherits the parent's full
effective mask, including its private nodes.

Binding eligibility
===================

A private binding's node is eligible while it is online private memory and the
allocating task's cpuset allows it.

While the node is eligible, the binding is strict: allocations use only that
node, including reserve and atomic allocations, and exhausting it leads to
private-memory OOM rather than common memory.

While the node is ineligible, the binding is suspended: allocations use the
task's allowed common memory. The binding resumes when the node becomes
eligible again. As with ordinary static bindings, policy queries continue to
report the original request.

Eligibility depends only on the node's state and the cpuset, never on free
capacity. An allocation that races with a cpuset update or memory hotplug is
retried before its binding is treated as suspended.

==============================  ==================  =========================
Event                           New allocations     Existing private pages
==============================  ==================  =========================
cpuset revokes the node         common memory       migrated to common memory
cpuset allows the node again    the private node    not migrated back
node is being hot-removed       the private node    migrated to common memory
node is removed                 common memory       (none remain)
node returns as private memory  the private node
node returns as common memory   that common node
==============================  ==================  =========================

While a node is being hot-removed, offlining isolates its free memory, so
faults can reach private-memory OOM. This matches an ordinary binding to a
single node that is being offlined.

Non-movable allocations and long-term pinning still follow zone restrictions.
A binding to a movable-only private node is ignored for allocations that cannot
use ``ZONE_MOVABLE``, which fall back to common memory, and long-term pins
migrate pages out of ``ZONE_MOVABLE`` as usual.

Cpuset-driven memory migration evacuates private pages to common memory only
when the cpuset revokes their node, and never uses private nodes as
destinations. In v2 this migration is enabled by default; legacy v1 controls it
with ``cpuset.memory_migrate``. Restoring a binding affects future allocations,
not the placement of pages that were already migrated.

Migration remains best-effort and can affect shared backing pages seen by other
tasks. Pages left behind remain accessible: cpusets restrict allocation
placement, not access to resident memory. Applications can also request explicit
migration, and memory hot-remove can migrate private pages to common memory.

Shared memory placement
=======================

``mbind(2)`` also supports private bindings on shmem-backed mappings, including
memfd, tmpfs and shared anonymous memory. The policy applies to the inode's
offset range, not independently to each mapper. Installing it through a tmpfs
``MAP_PRIVATE`` mapping also updates the inode policy; anonymous copy-on-write
pages have private ownership, but this does not make the inode policy
per-mapper.

Cpuset changes do not rebind an inode's shared policy to one mapper's cpuset.
Allocation uses the shared policy subject to the allocating task's current
cpuset and the node's current eligibility. An ineligible shared binding is
ignored for that allocation and common memory is used instead.

For example, two tasks can map the same memfd bound to private node P. A task
allowed to use P can allocate a previously absent page there. A task whose
cpuset excludes P allocates another absent page on common memory instead.
Both tasks subsequently see the same backing pages at those offsets, regardless
of their own allocation permissions. Accessing an existing shared page does not
reallocate it to follow the accessing task's policy. Cpuset-requested migration
can nevertheless move that backing page to common memory for all its mappers.
The object can therefore contain both private and common pages.

Ordinary shared bindings do not acquire private placement when a formerly
common node returns as private memory. If their stored mask has no common node
allowed by the allocating task, the allocator ignores the mask and retries on
common memory. This check also applies without an active cpuset configuration.

Private shmem pages remain charged to memory cgroups and are reclaimed and
swapped out like other shmem pages. Killing an allocating process on private OOM
does not necessarily free its inode-owned pages: a linked tmpfs file or an
object retained by another task can keep those pages alive. Applications must
manage the backing object's lifetime, truncate it or punch holes to release
capacity; unmapping alone does not guarantee that capacity is returned.

MM-service exclusion
====================

Reclaim and compaction act on a private node with userspace NUMA placement, as
placement requires. kswapd and kcompactd run for the node, its zones carry
watermarks, and memcg and hibernation reclaim reach it. Reclaim advice
(``MADV_COLD``, ``MADV_PAGEOUT`` and ``MADV_FREE``) and explicit ``reclaim``
and ``compact`` requests act on it as on any other node. Reclaim does not
demote private memory to another node. Reclaim requires compaction. A private
node that does not permit reclaim carries no watermarks, and explicit requests
for a service the node does not permit fail with ``-EINVAL``.

Private nodes are excluded from KSM merging, THP collapse, automatic NUMA
balancing, DAMON-native folio operations and memory-tier demotion. A KSM page
explicitly migrated to a private node remains shared, and may gain sharers
until ksmd rescans it. Generic exact-PFN contiguous allocation is restricted to
common memory. Private nodes hold no system reserve: their zones keep a small
fixed minimum watermark for allocations under private bindings, and their low
and high watermarks pace kswapd. Their free memory does not count toward
``MemAvailable``, the global dirty limits or hibernation image sizing. Page
cache that explicit migration places on a private node still counts toward
``MemAvailable``.

These restrictions do not prevent freeing memory, dropping mappings or explicit
migration. Memory hot-remove and soft offline can migrate resident folios to
common memory; pinned or otherwise unmovable pages can still prevent removal.

Observability and testing
=========================

``/sys/devices/system/node/has_memory`` reports online memory topology.
``has_common_memory`` reports the ordinary allocation pool and
``has_private_memory`` the private nodes; together they partition
``has_memory``. ``has_mpol_memory`` reports the nodes memory-policy interfaces
may target: every common node, and each private node whose owner allows
userspace placement, as kmem private mode does. Residency and memcg statistics
continue to account private pages.

The DAX selftests exercise private kmem hotplug, private bindings, explicit
migration, cpuset inheritance and rebinding, shared-policy placement across
mapper cpusets, common-to-private node retyping, reclaim and compaction on the
private node, and its exclusion from memory tiers and HugeTLB pool growth.
A VM can supply a device-dax range on a memory-only NUMA node for these tests.
