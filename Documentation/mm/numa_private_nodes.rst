.. SPDX-License-Identifier: GPL-2.0

====================
Private memory nodes
====================

A *private memory node* is a NUMA node whose memory is hotplugged by a driver
and deliberately hidden from the kernel's normal memory management.  Such a
node is an ``N_MEMORY`` node deliberately excluded from ``N_MEMORY_PUBLIC``
-- the set of nodes on the page allocator's fallback zonelists -- so it is
never considered by the normal or fallback allocation paths.

The intent is to give a driver a block of NUMA-addressable memory that the rest
of the kernel will not allocate from on its own, while still letting that memory
be mapped into processes as ordinary, struct-page, LRU-managed folios -- and to
let the driver re-enable individual mm services it is capable of allowing.

Preconditions
=============

A private node carries ``N_MEMORY`` but not ``N_MEMORY_PUBLIC``.  The backing
memory must come up on a node with no memory of its own; a node that already
holds public memory (already in ``N_MEMORY_PUBLIC``) cannot become private.

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

    Private nodes (``N_MEMORY`` nodes not in ``N_MEMORY_PUBLIC``) are
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
    so placement and migration share the same feature.

    Userspace placement onto a private node is restricted to ``MPOL_BIND``.
    A private node is reachable only through ``ZONELIST_PRIVATE``, which is
    confined solely by the policy nodemask, and only ``MPOL_BIND`` supplies
    that confining nodemask.  The soft/spreading modes (``MPOL_PREFERRED``,
    ``MPOL_PREFERRED_MANY``, ``MPOL_INTERLEAVE`` and
    ``MPOL_WEIGHTED_INTERLEAVE``) do not, so they are narrowed to the public
    nodes: a private node named alongside public ones is dropped from the
    set, and a policy that named nothing else fails with ``-EINVAL`` for an
    empty nodemask.  Narrowing rather than rejecting keeps ``numactl
    --interleave=all`` working once a device brings a private node online.
    A policy naming only public nodes is unaffected regardless of mode.

    The mempolicy component uses ``MPOL_F_PRIVATE`` at fault-time to select
    ``ZONELIST_PRIVATE`` and makes the node's memory available for allocation.
    It is otherwise an ordinary, relaxable mempolicy: an unsatisfiable request
    (an unmovable allocation on a movable-only private node) simply falls back.


Choosing a node state
=====================

``N_MEMORY`` says that memory exists on a node, and nothing more.  Three
related questions have separate answers:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Question
     - State
   * - Does memory exist here?  (reporting, accounting)
     - ``N_MEMORY``
   * - Can an allocation that named no node land here?
     - ``N_MEMORY_PUBLIC``
   * - May a given mm service operate on the node?
     - ``N_MEMORY_<SERVICE>``

Test ``N_MEMORY`` to report or account for memory, where a private node's
pages count like any other.  Where the question is whether an allocation
could have landed somewhere, or whether a nodemask already covers everything
an unbound allocation can reach, test ``N_MEMORY_PUBLIC``: ``N_MEMORY``
also holds the private nodes, which no unbound allocation can use.


cpuset interaction
==================

cpuset.mems governs every ``N_MEMORY`` node, private nodes included, exactly as
it governs public ones.  Adding a private node to a cgroup's ``cpuset.mems``
grants that cgroup use of it (allocation, user placement, and kernel demotion);
removing it revokes that use.

The one restriction is that a non-empty ``cpuset.mems`` must keep at least one
public node.  A private-only set is rejected, because a private node
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

  __add_memory_driver_managed(nid, start, size, resource_name,
                              mhp_flags, online_type, features)

which onlines the range and claims the node's feature mask (see below).  A
mask without ``NODE_MEMORY_FEAT_PUBLIC`` makes the node private;
``NODE_MEMORY_FEAT_ALL`` is ordinary system RAM.

The mask is fixed for as long as the node holds memory, so a second range
added to the same node has to name the mask the node already carries.  In
practice that makes a private node the property of one driver.

The node leaves ``N_MEMORY`` only when its last range is offlined.

.. kernel-doc:: mm/memory_hotplug.c
   :identifiers: __add_memory_driver_managed

.. kernel-doc:: drivers/base/node.c
   :identifiers: node_memory_features_register node_memory_features_unregister

Features (per-service opt-ins)
==============================

Because the default is "no mm service touches the node", each service a driver
wants back is requested explicitly through a feature bit in the ``features``
argument to ``node_memory_features_register()``, which
``__add_memory_driver_managed()`` passes on its behalf.  The mm side tests the
matching ``N_MEMORY_<FEATURE>`` state before acting - directly via
``node_state()``, while iterating via ``for_each_node_state()`` /
``for_each_zone_node_state()``, or per folio via ``folio_allows_mm_op()``:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Feature
     - Re-enables
   * - ``NODE_MEMORY_FEAT_RECLAIM``
     - reclaim of the node's folios, by the mm and by userspace
       ``MADV_COLD`` / ``PAGEOUT`` / ``FREE`` (userland-driven reclaim)
   * - ``NODE_MEMORY_FEAT_USER_NUMA``
     - all userspace-directed placement and migration: ``mbind()`` /
       ``set_mempolicy()`` / home node, ``move_pages()`` /
       ``migrate_pages()`` to/from the node, and HugeTLB pool placement
       (every route into the pool names a node from userspace)
   * - ``NODE_MEMORY_FEAT_DEMOTION``
     - reclaim-driven tiering demotion onto the node (the node joins the
       demotion hierarchy)
   * - ``NODE_MEMORY_FEAT_NUMA_BALANCING``
     - access-based NUMA balancing scan/migration of the node's folios
   * - ``NODE_MEMORY_FEAT_LTPIN``
     - ``FOLL_LONGTERM`` GUP pins
   * - ``NODE_MEMORY_FEAT_DAMON``
     - DAMON monitoring and DAMOS actions on the node's folios
   * - ``NODE_MEMORY_FEAT_KSM``
     - KSM merging of the node's folios
   * - ``NODE_MEMORY_FEAT_COLLAPSE``
     - THP collapse of the node's folios, by khugepaged and ``MADV_COLLAPSE``

``NODE_MEMORY_FEAT_PUBLIC`` is what makes a node ordinary: a node with it
set holds every feature, and its absence is what makes a node private.

Dependencies between features are enforced **once**, by
``node_memory_features_register()`` at hotplug, rather than by whatever sets
the bits:

* ``DEMOTION`` requires ``RECLAIM`` (a demotion target accumulates demoted
  pages, so without reclaim as a safety valve it would just fill up).

Feature flags are expected to be stable at runtime.

Observability
=============

Userspace is given the two distinctions it can act on, as ordinary node-state
nodelists alongside ``has_memory`` and ``has_cpu``:

* ``/sys/devices/system/node/has_user_memory`` -- the nodes userspace may place
  memory on.  A memory node absent from this list cannot be named in a
  mempolicy or migration call.
* ``/sys/devices/system/node/has_public_memory`` -- the nodes an allocation
  reaches without naming them.  A memory node absent from this list only ever
  receives explicitly bound memory, so its capacity must not be counted towards
  what an unbound allocation can obtain.

A private node is one that is in ``has_memory`` but not in
``has_public_memory``; it may still be in ``has_user_memory``, which is the
difference between a device tier a task can bind to and one it cannot touch.

The full ``NODE_MEMORY_FEAT_*`` mask is not published anywhere: the bit
layout is kernel-internal and would otherwise become ABI, and nothing outside
the node that owns it needs the individual bits.  Whoever provisioned a node
already knows the mask it asked for -- ``private_node=`` on the command line,
or the driver that called ``__add_memory_driver_managed()`` -- and a mask that
would not have been honoured is refused outright rather than adjusted, so a
node that came up private carries exactly the mask that was requested.

Otherwise a private node is reported like any other:

* ``/proc/<pid>/numa_maps`` -- per-node residency includes private nodes
* ``/proc/kcore`` -- private-node RAM appears in the kcore RAM map
* memcg per-node statistics account private-node memory.

Testing
=======

``dax_test`` (``drivers/dax/test.c``) is a provider that hands a physical
range to dax/kmem with a chosen feature mask, so the matrix can be exercised
without a real device::

  modprobe dax_test range_start=<pa> range_size=<bytes> target_node=<nid> \
           features=<mask>[,<mask>...]

Each mask creates one DAX device on ``target_node``, splitting the range
between them, which lets a single node be offered two different masks.  The
mask reaches the node through ``__add_memory_driver_managed()`` like any other
provider's, so the same rules apply: the first one wins, an equal mask joins,
and a different mask is refused.

Mapping the memory into a process needs the node named at fault time, which
``dax_test`` provides through debugfs::

  <debugfs>/dax_test/anon       mmap() yields anonymous memory bound to the node
  <debugfs>/dax_test/bind_node  set the node the next mapping binds to

Boot-time provisioning uses ``private_node=`` (see `Boot memory`_ above).

KTAP selftests live in ``tools/testing/selftests/dax/`` (``private_node_*``).
