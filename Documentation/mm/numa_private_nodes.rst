.. SPDX-License-Identifier: GPL-2.0

====================
Private memory nodes
====================

A *private memory node* is a NUMA node whose memory is hotplugged by a driver
and deliberately hidden from the kernel's normal memory management.  Such a
node is an ``N_MEMORY`` node deliberately excluded from ``N_MEMORY_FALLBACK``
-- the set of nodes on the page allocator's fallback zonelists -- so it is
never considered by the normal or fallback allocation paths.

The intent is to give a driver a block of NUMA-addressable memory that the rest
of the kernel will not allocate from on its own, while still letting that memory
be mapped into processes as ordinary, struct-page, LRU-managed folios -- and to
let the driver re-enable individual mm services it is capable of allowing.

Preconditions
=============

A private node carries ``N_MEMORY`` but not ``N_MEMORY_FALLBACK``.  The backing
memory must come up on a node with no memory of its own; a node that already
holds public memory (already in ``N_MEMORY_FALLBACK``) cannot become private.

In practice the memory is provided by a device driver or a DAX device whose
target node has no other memory, and usually no CPUs.

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

    Private nodes (``N_MEMORY`` nodes not in ``N_MEMORY_FALLBACK``) are
    **excluded** from both ``FALLBACK`` and ``NOFALLBACK`` zonelists.  Instead
    they are reachable through ``ZONELIST_PRIVATE``, which contains every
    ``N_MEMORY`` node (public and private).  This is
    the only zonelist that contains private-node zones, and so the only way
    to acquire private node allocations is to explicitly request that zonelist.

    Even an allocation carrying ``__GFP_THISNODE`` cannot access the node's
    memory without also explicitly passing the private zonelist.  This prevents
    incidental allocation of private memory by users of possible/online
    nodelists.

    When ``CONFIG_NUMA`` is disabled ``ZONELIST_PRIVATE`` aliases
    ``ZONELIST_FALLBACK`` and is never selected.

The user_numa path

    ``MPOL_F_PRIVATE`` is an internal user_numa flag (never accepted from
    userspace) marking that a mempolicy has a private node in its nodemask.

    When ``N_MEMORY_USER_NUMA`` for a private node is set, user-sourced mempolicy
    (``set_mempolicy(2)``) and migration (``move_pages(2)``) operations are
    allowed to include that node in nodemasks and targets respectively.

    ``mbind(MPOL_MF_MOVE)`` is both a mempolicy and a migration operation,
    so placement and migration share the same capability.

    Userspace placement onto a private node is restricted to ``MPOL_BIND``.
    A private node is reachable only through ``ZONELIST_PRIVATE``, which is
    confined solely by the policy nodemask, and only ``MPOL_BIND`` supplies
    that confining nodemask.  The soft/spreading modes (``MPOL_PREFERRED``,
    ``MPOL_PREFERRED_MANY``, ``MPOL_INTERLEAVE`` and
    ``MPOL_WEIGHTED_INTERLEAVE``) do not, so a policy of one of those modes
    that names a private node is rejected with ``-EINVAL``.  A policy naming
    only public nodes is unaffected regardless of mode.

    The mempolicy component uses ``MPOL_F_PRIVATE`` at fault-time to select
    ``ZONELIST_PRIVATE`` and makes the node's memory available for allocation.
    It is otherwise an ordinary, relaxable mempolicy: an unsatisfiable request
    (an unmovable allocation on a movable-only private node) simply falls back.


cpuset interaction
==================

cpuset.mems governs every ``N_MEMORY`` node, private nodes included, exactly as
it governs public ones.  Adding a private node to a cgroup's ``cpuset.mems``
grants that cgroup use of it (allocation, user placement, and kernel demotion);
removing it revokes that use.

The one restriction is that a non-empty ``cpuset.mems`` must keep at least one
fallback (public) node.  A private-only set is rejected, because a private node
cannot back the allocations ineligible for it (everything but an explicit
``MPOL_F_PRIVATE`` bind, plus all unmovable allocations); this is the node
analogue of a ``ZONE_MOVABLE``-only cpuset.

On a ``cpuset.mems`` change only the userland-NUMA subset (``N_MEMORY_USER_NUMA``
nodes: public nodes and opted-in private nodes) is positionally remapped or
migrated.  A device-owned (non-``USER_NUMA``) private node is never positionally
remapped and never migrated to or from: a cpuset change can drop it from a policy
but can never fold a policy onto one it never named.


Provisioning
============

A driver brings memory up as private with::

  add_private_memory_driver_managed(nid, start, size, resource_name,
                                    mhp_flags, online_type, np)

which onlines the range and registers the driver-owned ``struct node_private``
(``np``) describing the node, including its capability bitmap (see below).

Only one driver/service may register a ``struct node_private``, which
heavily implies a "one-node-per-device" design of the system.

The node leaves ``N_MEMORY`` only when its last range is offlined.

.. kernel-doc:: mm/memory_hotplug.c
   :identifiers: __add_memory_driver_managed

.. kernel-doc:: drivers/base/node.c
   :identifiers: node_private_register node_private_unregister

Boot memory
-----------

Memory the boot path onlines never reaches that call - ``free_area_init()``
publishes it as system RAM before any driver runs - so a CXL window the BIOS
reports in E820 and the SRAT is never offered to dax/kmem.  Name it instead::

  private_node=<nid>[,<caps>]

``caps`` defaults to 0 and follows the rules below.  The kernel owns the
resulting ``struct node_private``, so a driver cannot later claim the node.
Refused if the node owns CPUs or the kernel image.

Capabilities (per-service opt-ins)
==================================

Because the default is "no mm service touches the node", each service a driver
wants back is requested explicitly through a capability bit in
``np->caps``.  The mm side checks the matching ``node_allows_*()`` /
``folio_allows_*()`` predicate before acting:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Capability
     - Re-enables
   * - ``NODE_MEMORY_CAP_RECLAIM``
     - reclaim of the node's folios, by the mm and by userspace
       ``MADV_COLD`` / ``PAGEOUT`` / ``FREE`` (userland-driven reclaim)
   * - ``NODE_MEMORY_CAP_USER_NUMA``
     - all userspace-directed placement and migration: ``mbind()`` /
       ``set_mempolicy()`` / home node, and ``move_pages()`` /
       ``migrate_pages()`` to/from the node
   * - ``NODE_MEMORY_CAP_DEMOTION``
     - reclaim-driven tiering demotion onto the node (the node joins the
       demotion hierarchy)
   * - ``NODE_MEMORY_CAP_NUMA_BALANCING``
     - access-based NUMA balancing scan/migration of the node's folios
   * - ``NODE_MEMORY_CAP_LTPIN``
     - ``FOLL_LONGTERM`` GUP pins
   * - ``NODE_MEMORY_CAP_DAMON``
     - DAMON monitoring and DAMOS actions on the node's folios
   * - ``NODE_MEMORY_CAP_KSM``
     - KSM merging of the node's folios
   * - ``NODE_MEMORY_CAP_COLLAPSE``
     - THP collapse of the node's folios, by khugepaged and ``MADV_COLLAPSE``
   * - ``NODE_MEMORY_CAP_HUGETLB``
     - HugeTLB allocation from the node

``NODE_MEMORY_CAP_FALLBACK`` is the public marker: a node with it set is an
ordinary node holding every feature, and its absence is what makes a node
private.

Dependencies between capabilities are enforced **once**, by
``node_private_register()`` at hotplug, rather than by whatever sets the bits:

* ``DEMOTION`` requires ``RECLAIM`` (a demotion target accumulates demoted
  pages, so without reclaim as a safety valve it would just fill up).

Capability flags are expected to be stable at runtime.

Observability
=============

Userspace is given the two distinctions it can act on, as ordinary node-state
nodelists alongside ``has_memory`` and ``has_cpu``:

* ``/sys/devices/system/node/has_user_memory`` -- the nodes userspace may place
  memory on.  A memory node absent from this list cannot be named in a
  mempolicy or migration call.
* ``/sys/devices/system/node/has_fallback_memory`` -- the nodes an allocation
  reaches without naming them.  A memory node absent from this list only ever
  receives explicitly bound memory, so its capacity must not be counted towards
  what an unbound allocation can obtain.

A private node is one that is in ``has_memory`` but not in
``has_fallback_memory``; it may still be in ``has_user_memory``, which is the
difference between a device tier a task can bind to and one it cannot touch.

The full ``NODE_MEMORY_CAP_*`` mask is not in sysfs -- the bit layout is
kernel-internal and would otherwise become ABI.  It is available for tests and
debugging at ``<debugfs>/node/mem_features``, one ``<nid> <mask>`` line per
online node.

Otherwise a private node is reported like any other:

* ``/proc/<pid>/numa_maps`` -- per-node residency includes private nodes
* ``/proc/kcore`` -- private-node RAM appears in the kcore RAM map
* memcg per-node statistics account private-node memory.

Testing
=======

The ``dax_kmem`` driver (``drivers/dax/kmem.c``) exposes a capability mask per
device at ``<debugfs>/dax_kmem/daxX.Y/mm_capabilities`` that selects how the DAX
device's memory is hotplugged.  ``NODE_MEMORY_CAP_ALL`` (``~0``, the default)
brings it up as an ordinary N_MEMORY node; any other value brings it up as a
private node whose set bits are the only services it opts into.  It is writable
only while the device is unplugged (``-EBUSY`` otherwise), and a mask with
``FALLBACK`` but not ``USER_NUMA`` is rejected: a public node has to be
user-targetable.  Dependencies such as DEMOTION requiring RECLAIM are validated
at hotplug rather than at write time, so an inconsistent mask is accepted by the
write and fails the subsequent online.

The knob is in debugfs, not sysfs, because the value is a raw in-kernel bit
layout that should not become ABI.  A deployment that wants a private node
describes it in firmware or with the ``private_node=`` command-line parameter;
this exists so the capability matrix can be exercised on a running kernel.

The ``dax_file`` attribute additionally opts the kmem's ``/dev/daxX.Y`` cdev
into being mmap-capable, which faults ordinary anonymous memory bound to the
node, which is how a test maps private-node memory into a process (see
Documentation/ABI/testing/sysfs-bus-dax).

KTAP selftests live in ``tools/testing/selftests/dax/`` (``private_node_*``).
