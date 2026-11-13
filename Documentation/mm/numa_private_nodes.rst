.. SPDX-License-Identifier: GPL-2.0

====================
Private memory nodes
====================

A private memory node has online System RAM (``N_MEMORY``), but is absent from
``N_MEMORY_COMMON`` and present in ``N_MEMORY_PRIVATE``. Its capacity is not
available to ordinary allocations, and generic memory management services do
not operate on it. This lets a driver expose specialized memory, such as HBM,
as NUMA memory without making it a general allocation fallback.

Provisioning
============

Boot memory on the nodes named by the ``private_nodes=`` kernel parameter comes
up private. Such a node joins ``N_MEMORY_PRIVATE`` when boot publishes its
memory, and leaves it when its last memory goes offline.

Private memory requires ``CONFIG_NUMA``, and a private node must not have CPUs,
since a CPU's local memory serves ordinary allocations. ``private_nodes=`` is
x86 only, the only architecture that implements the early CPU check. A listed
node with a possible CPU, the kernel image, a CMA area or no memory stays
common. ``kernelcore=`` and ``movablecore=`` size ``ZONE_MOVABLE`` over every
node, so ``private_nodes=`` is ignored with them. A CPU hot-added to a node
with private memory cannot be onlined, and hot-adding memory to such a node
fails with ``-EBUSY``.

Early boot allocations that name no node prefer other nodes to the listed ones,
but fall back to them rather than fail. A private boot node can therefore hold
kernel memory that stays reserved: its own metadata, such as its memory map,
those fallbacks, and allocations made before the firmware NUMA tables are
parsed. Gigantic HugeTLB boot pages and ``hugetlb_cma`` skip private nodes.

Each node records an owner cookie beside its features. A driver that allocates
from its private memory is identified by an address it owns, which the kernel
only compares and never dereferences. Private boot memory, which no driver
allocates from, and all common memory are owned by
``NODE_MEMORY_OWNER_KERNEL``.

Ordinary kernel allocations require common memory, so a system must keep at
least one common-memory node. The kernel does not enforce this; boot memory
normally provides it.

Allocation
==========

A private node is absent from every node's fallback zonelist, and from its own
``__GFP_THISNODE`` zonelist, so neither an allocation that names no node nor a
node-local kernel allocation can land on it. An allocation that names a private
node without ``__GFP_THISNODE`` falls back to common memory through the node's
fallback zonelist.

Each node also has a private zonelist, which holds the node's own zones only if
the node is private. Memory management code reaches private memory only through
it, by passing ``ALLOC_ZONELIST_PRIVATE`` with a nodemask. Such a request is
strict while its node is eligible, and is relaxed onto common memory once a
cpuset update or memory hot-remove makes the node ineligible.

Drivers allocate private memory with ``folio_alloc_private()``, naming the node
and its owner. The allocation is strict: it fails rather than falling back to
common memory, and never invokes the OOM killer, so the owner must handle
exhaustion of its memory. The caller's cpuset still applies.

Generic exact-PFN contiguous allocation refuses private memory with
``-EPERM``.

Userspace
=========

Memory policies, ``move_pages(2)`` and ``migrate_pages(2)`` cannot place memory
on a private node. Private nodes in a ``set_mempolicy(2)`` or ``mbind(2)`` mask
are ignored, as nodes outside the cpuset are, and ordinary policies rebind over
the cpuset's nodes other than private ones. ``move_pages(2)`` rejects a private
destination with ``-ENODEV``, and ``migrate_pages(2)`` ignores private
destination nodes.

``cpuset.mems`` may include private nodes, but an effective mask must contain
common memory to serve ordinary allocations. Cgroup v2 and
``cpuset_v2_mode`` keep a private-only request and add the parent's common
nodes to its effective mask. Legacy v1 rejects a private-only mask, or attaching
a task to one, with ``-ENOSPC``, and hot-unplug of the last common node empties
a legacy cpuset's mask and moves its tasks to an ancestor. A literally empty v2
request inherits the parent's full effective mask.

MM-service exclusion
====================

Reclaim and compaction skip private nodes. kswapd and kcompactd do not run for
them, direct reclaim and compaction walk only common zones, and explicit
``nodeN/reclaim`` and ``nodeN/compact`` requests fail with ``-EINVAL``. Private
nodes hold no system reserve, and their zones carry no watermarks.

Private nodes are also excluded from KSM merging, THP collapse, automatic NUMA
balancing, DAMON-native folio operations, memory-tier demotion, HugeTLB pool
growth and large system hash distribution.

Tasks whose cpusets share only private nodes are not eligible OOM victims for
each other. OOM victim selection still scores a task by all of its resident
memory, private pages included.

Requests that act on individual folios do not check the node type: reclaim
advice (``MADV_COLD``, ``MADV_PAGEOUT``, ``process_madvise(2)``) and DAMON
virtual-address schemes would act on private folios on the LRU, but no kernel
code puts private folios on the LRU yet. Free page reporting reports free
private pages like any others.

Memory hot-remove migrates resident folios off a node being offlined onto
common memory.

Accounting
==========

Each memory counter counts the memory that its own kind of usage can reach.

``totalram_pages()`` and ``nr_free_pages()``, which ``MemTotal`` and
``MemFree`` report, count only general-purpose memory, memory that holds
ordinary data, which is common memory. The overcommit limit, the OOM capacity
and every other limit or default sized from ``totalram_pages()`` follow. A
node's features are fixed while it has memory, so the count stays exact as
memory comes and goes.

``MemAvailable`` and the global dirty limits count free pages only on common
nodes, since free private memory serves only allocations by the node's owner.

The kernel reserves, the watermark budget, large system hash sizing and the
auto-movable online policy's zone ratio count only common memory, since kernel
allocations cannot use any other memory.
Hibernation likewise counts only common free memory as room for image pages.
The vmstat counters, including ``nr_free_pages``
in ``/proc/vmstat``, per-node counters, ``nodeN/meminfo`` and memory cgroup
usage count all memory, private memory included.

Observability and testing
=========================

``/sys/devices/system/node/has_memory`` reports online memory topology.
``has_common_memory`` reports the ordinary allocation pool and
``has_private_memory`` the private nodes; together they partition
``has_memory``.

``CONFIG_PRIVATE_NODE_KUNIT_TEST`` checks the node-state partition and the
zonelist invariants above against the running system. With a kernel-owned
private node, such as one from ``private_nodes=``, it also checks
``folio_alloc_private()`` and where allocations that name the node land.
