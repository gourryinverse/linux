.. SPDX-License-Identifier: GPL-2.0

====================
Private memory nodes
====================

A *private memory node* is a NUMA node whose memory is hotplugged by a driver
and deliberately hidden from the kernel's normal memory management.  Such a
node is marked ``N_MEMORY_PRIVATE`` instead of ``N_MEMORY``; the two states
are mutually exclusive, so a private node is never considered by the page
allocator's normal or fallback paths.

The intent is to give a driver a block of NUMA-addressable memory that the rest
of the kernel will not allocate from on its own, while still letting that memory
be mapped into processes as ordinary, struct-page, LRU-managed folios -- and to
let the driver re-enable individual mm services it is capable of allowing.

Preconditions
=============

``N_MEMORY_PRIVATE`` and ``N_MEMORY`` are mutually exclusive, so the backing
memory must come up on a node that has no DRAM of its own (otherwise the node
would already be ``N_MEMORY``).

In practice the memory is provided by a device driver or a DAX device whose
target node has no other memory, and usually no CPUs.

Isolation model
===============

Isolation is *opt-in by exclusion* and is **structural**: by default nothing in
the kernel can place memory on a private node because the node is absent from the
zonelists an ordinary allocation walks.

Zonelist exclusion
    The kernel page allocator depends on the ``FALLBACK`` and ``NOFALLBACK``
    zonelists to allocate memory.  A normal ``N_MEMORY`` node's zones (except
    ``ZONE_DEVICE``) appear in these lists and allow allocations to fall-back
    to less preferable locations if the preferred location is pressured.

    ``__GFP_THISNODE`` is used during normal operation to switch between
    ``FALLBACK`` and ``NOFALLBACK``, where ``NOFALLBACK`` only contains the
    zonelists of the preferred node.

    ``N_MEMORY_PRIVATE`` nodes are **excluded** from both ``FALLBACK`` and
    ``NOFALLBACK`` zonelists.  Instead they are added to ``ZONELIST_PRIVATE``,
    which includes both ``N_MEMORY`` and ``N_MEMORY_PRIVATE`` nodes.  This is
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

    When ``CAP_USER_NUMA`` for a private node is set, user-sourced mempolicy
    (``set_mempolicy(2)``) and migration (``move_pages(2)``) operations are
    allowed to include that node in nodemasks and targets respectively.

    ``mbind(MPOL_MF_MOVE)`` is both a mempolicy and a migration operation,
    so placement and migration share the same capability.

    The mempolicy component uses ``MPOL_F_PRIVATE`` at fault-time to select
    ``ZONELIST_PRIVATE`` and makes the node's memory available for allocation.
    It is otherwise an ordinary, relaxable mempolicy: an unsatisfiable request
    (an unmovable allocation on a movable-only private node) simply falls back.


cpuset interaction
==================

cpuset.mems does **not** partition private nodes.  cpuset neither grants nor
denies access, and rebinding cpuset.mems nodemasks do not affect a private node's
residency in any nodemask.

Likewise, a private node's inclusion in a nodemask does not affect cpuset.mems'
filtering of any ``N_MEMORY`` - they remain partitioned according to cpuset.


Provisioning
============

A driver brings memory up as private with::

  add_private_memory_driver_managed(nid, start, size, resource_name,
                                    mhp_flags, online_type, np)

which onlines the range and registers the driver-owned ``struct node_private``
(``np``) describing the node, including its capability bitmap (see below).

Only one driver/service may register a ``struct node_private``, which
heavily implies a "one-node-per-device" design of the system.

The node leaves ``N_MEMORY_PRIVATE`` only when the last range is offlined.

.. kernel-doc:: mm/memory_hotplug.c
   :identifiers: __add_memory_driver_managed

.. kernel-doc:: drivers/base/node.c
   :identifiers: node_private_register node_private_unregister

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
   * - ``NODE_PRIVATE_CAP_RECLAIM``
     - reclaim of the node's folios, by the mm and by userspace
       ``MADV_COLD`` / ``PAGEOUT`` / ``FREE`` (userland-driven reclaim)
   * - ``NODE_PRIVATE_CAP_USER_NUMA``
     - all userspace-directed placement and migration: ``mbind()`` /
       ``set_mempolicy()`` / home node, and ``move_pages()`` /
       ``migrate_pages()`` to/from the node
   * - ``NODE_PRIVATE_CAP_HOTUNPLUG``
     - hot-unplug via migration
   * - ``NODE_PRIVATE_CAP_DEMOTION``
     - reclaim-driven tiering demotion onto the node (the node joins the
       demotion hierarchy)
   * - ``NODE_PRIVATE_CAP_NUMA_BALANCING``
     - access-based NUMA balancing scan/migration of the node's folios
   * - ``NODE_PRIVATE_CAP_LTPIN``
     - ``FOLL_LONGTERM`` GUP pins

khugepaged never operates on private-node folios (like ZONE_DEVICE), and DAMON
does not act on them; ``MADV_COLLAPSE`` is covered by ``CAP_USER_NUMA``.

Dependencies between capabilities are enforced **once**, by
``node_private_register()`` at hotplug, rather than by whatever sets the bits:

* ``DEMOTION`` requires ``RECLAIM`` (a demotion target accumulates demoted
  pages, so without reclaim as a safety valve it would just fill up).

Capability flags are expected to be stable at runtime.

Observability
=============

A private node is reported through:

* ``/sys/devices/system/node/has_private_memory``
* ``/proc/<pid>/numa_maps`` -- per-node residency includes private nodes
* ``/proc/kcore`` -- private-node RAM appears in the kcore RAM map
* memcg per-node statistics account private-node memory.
