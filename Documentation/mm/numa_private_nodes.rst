.. SPDX-License-Identifier: GPL-2.0

====================
Private memory nodes
====================

A *private memory node* is a NUMA node whose memory is deliberately hidden from
the kernel's normal memory management.  Such a node is an ``N_MEMORY`` node
deliberately excluded from ``N_MEMORY_COMMON`` -- the set of nodes on the page
allocator's fallback zonelists -- so it is never considered by the normal or
fallback allocation paths.

The intent is to give a driver a block of NUMA-addressable memory that the rest
of the kernel will not allocate from on its own, while still letting that memory
be mapped into processes as ordinary, struct-page, LRU-managed folios -- and to
let the driver re-enable individual mm services it is capable of allowing.

Preconditions
=============

A private node carries ``N_MEMORY`` but not ``N_MEMORY_COMMON``.  The backing
memory must come up on a node with no memory of its own; a node that already
holds common memory (already in ``N_MEMORY_COMMON``) cannot become private.

Normally the memory is provided by a device driver or a DAX device whose target
node has no other memory, and usually no CPUs.

Isolation model
===============

Isolation is **structural** and rests on exactly one mechanism, the
page-allocator zonelists: by default nothing in the kernel can place memory on a
private node because the node is absent from the zonelists an ordinary allocation
walks.

Zonelist exclusion
    The kernel page allocator depends on the ``FALLBACK`` and ``NOFALLBACK``
    zonelists to allocate memory.  A normal ``N_MEMORY`` node's zones (except
    ``ZONE_DEVICE``) appear in these lists and allow allocations to fall-back
    to less preferable locations if the preferred location is pressured.

    ``__GFP_THISNODE`` is used during normal operation to switch between
    ``FALLBACK`` and ``NOFALLBACK``, where ``NOFALLBACK`` only contains the
    zonelists of the preferred node.

    Private nodes (``N_MEMORY`` nodes not in ``N_MEMORY_COMMON``) are
    **excluded** from both ordinary lists.  Each private node has a separate
    no-fallback zonelist containing only that node.

    ``folio_alloc_node_private()`` selects this list.  It requires
    ``__GFP_THISNODE``, so an allocation can reach only the private node
    explicitly named by its caller.  Allocation failure is returned to the
    caller; the interface does not invoke the OOM killer or fall back to
    another node.

    A caller that already knows an exact PFN range can pass
    ``ACR_FLAGS_PRIVATE`` to ``alloc_contig_range()``.  Without that explicit
    flag, contiguous range allocation is restricted to common memory.  The
    node-searching ``alloc_contig_pages()`` interface searches only ordinary
    zonelists and therefore remains common-only.

    When ``CONFIG_NUMA`` is disabled the private allocator is unavailable.

Userspace NUMA policy does not provide access to private memory.  A driver may
map private folios into a process, but ordinary faults, mempolicy, migration,
HugeTLB allocation and automatic NUMA balancing remain confined to common
memory.


Choosing a node state
=====================

``N_MEMORY`` says that memory exists on a node, and nothing more.  The
following questions have separate answers:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Question
     - State
   * - Does memory exist here?  (reporting, accounting)
     - ``N_MEMORY``
   * - Can an allocation that named no node land here?
     - ``N_MEMORY_COMMON``
   * - May compaction operate on the node?
     - ``N_MEMORY_COMPACTION``
   * - May generic reclaim operate on the node?
     - ``N_MEMORY_RECLAIM``

Test ``N_MEMORY`` to report or account for memory, where a private node's
pages count like any other.  Where the question is whether an allocation
could have landed somewhere, or whether a nodemask already covers everything
an unbound allocation can reach, test ``N_MEMORY_COMMON``: ``N_MEMORY``
also holds the private nodes, which no unbound allocation can use.


cpuset interaction
==================

cpuset.mems continues to describe every ``N_MEMORY`` node, private nodes
included, but does not itself provide an allocation interface for private
memory.  Ordinary allocations always require effective common memory.

A private-only requested set is handled according to the cpuset mode.  Cgroup
v2 and ``cpuset_v2_mode`` retain the requested ``cpuset.mems`` value.  Its
effective mask keeps requested private nodes granted by the parent and adds the
parent's common nodes, without inheriting unrequested private nodes.  Legacy v1
has no effective-mems fallback, so setting a private-only mask, or attaching a
task to a cpuset with one, fails with ``-ENOSPC``.  If hot-unplug removes the
last common node from a legacy cpuset, its mask becomes empty and its tasks are
moved to an ancestor.

Provisioning
============

A driver brings memory up as private with::

  __add_memory_driver_managed(nid, start, size, resource_name,
                              mhp_flags, online_type, features)

which onlines the range and claims the node's feature mask (see below).  A
mask without ``NODE_MEMORY_FEAT_COMMON`` makes the node private;
``NODE_MEMORY_FEAT_ALL`` is ordinary system RAM.

The mask is fixed for as long as the node holds memory, so a second range
added to the same node has to name the mask the node already carries.  In
practice that makes a private node the property of one driver.

The node leaves ``N_MEMORY`` only when its last range is offlined.

.. kernel-doc:: mm/memory_hotplug.c
   :identifiers: __add_memory_driver_managed

.. kernel-doc:: drivers/base/node.c
   :identifiers: node_features_register node_features_unregister

Private-node capabilities
=========================

Private memory is excluded from opportunistic folio walkers, including KSM,
THP collapse, DAMON-native monitoring, filtering, statistics and migration,
and automatic NUMA balancing.  Those services treat a private-node folio like
a ``ZONE_DEVICE`` folio and have no private-node opt-in.  Madvise-backed DAMOS
actions delegate folio handling to the underlying MM service and follow that
service's eligibility rules.

Automatic MM services are selected through feature bits in the ``features``
argument to ``node_features_register()``, which
``__add_memory_driver_managed()`` passes on the driver's behalf:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Feature
     - Re-enables
   * - ``NODE_MEMORY_FEAT_COMPACTION``
     - direct, background, proactive and explicitly requested compaction
   * - ``NODE_MEMORY_FEAT_RECLAIM``
     - direct, background, proactive, and userspace-requested reclaim of the
       node's folios
   * - ``NODE_MEMORY_FEAT_USER_WRITE``
     - userspace may write resident folios in place.  A driver may withhold
       this feature from a movable-only node; anonymous folios on such a node
       remain read-only and a write fault copies them to common memory.

``NODE_MEMORY_FEAT_COMMON`` is what makes a node ordinary: a node with it
set holds every feature, and its absence is what makes a node private.

Feature flags are expected to be stable at runtime.

Anonymous DAX
=============

``CONFIG_DEV_DAX_ANON`` provides an explicit userspace allocation interface for
private device memory.  Binding a device-dax instance to the ``anondax`` driver
onlines its memory as a private node and exposes the existing ``/dev/daxX.Y``
device with anondax file operations.

Only ``MAP_SHARED`` mappings are accepted.  Each open file is an independent
allocation object; mappings of the same offsets through that file share their
contents, including across ``fork()``.  Separate opens do not share contents.
Faults allocate zeroed pages directly from the device's private node with
``folio_alloc_node_private()``.  They never fall back to common memory, and an
allocation failure raises ``SIGBUS`` in the faulting process.

Anondax always enables reclaim and compaction for its private node; there are no
feature toggles.  Its mapped pages are owned by the open file rather than placed
on an LRU, so those pages are not themselves reclaimable or migratable.  Closing
the last reference to the file frees them.  The standard memory-hotplug sysfs
interface can then offline the range, and unbinding an unused device removes it.

Anondax has no feature-control ABI.  A device is selected explicitly by writing
its name to ``/sys/bus/dax/drivers/anondax/new_id``.

Observability
=============

Userspace is given the common-memory distinction as an ordinary node-state
nodelist alongside ``has_memory`` and ``has_cpu``:

* ``/sys/devices/system/node/has_common_memory`` -- the nodes an allocation
  reaches without naming them.  A memory node absent from this list only ever
  receives memory from its owning driver, so its capacity must not be counted
  towards what an ordinary allocation can obtain.

A private node is one that is in ``has_memory`` but not in
``has_common_memory``.

The full ``NODE_MEMORY_FEAT_*`` mask is not published anywhere: the bit layout
is kernel-internal and would otherwise become ABI, and nothing outside the node
that owns it needs the individual bits.  The provisioning driver already knows
the mask it passed to ``__add_memory_driver_managed()``.  An unsupported request
is refused rather than adjusted, so a private node carries exactly what its
owner requested.

Otherwise a private node is reported like any other:

* ``/proc/<pid>/numa_maps`` -- per-node residency includes private nodes
* ``/proc/kcore`` -- private-node RAM appears in the kcore RAM map
* memcg per-node statistics account private-node memory.

For development, a virtual machine can expose a persistent-memory range on a
memory-only NUMA node.  Converting that range to device-dax and binding it to
``anondax`` or ``cramdax`` exercises private-node provisioning without special
boot-time node classification.  At least one ordinary common-memory node must
remain for kernel allocations.
