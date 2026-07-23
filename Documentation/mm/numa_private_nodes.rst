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
node has no other memory, and usually no CPUs.  The ``private_node=`` boot
parameter described below is available for development and early bring-up.

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
    **excluded** from both ``FALLBACK`` and ``NOFALLBACK`` zonelists.  Instead
    they are reachable through ``ZONELIST_PRIVATE``, which contains every
    ``N_MEMORY`` node (common and private).  This is
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
    ``MPOL_WEIGHTED_INTERLEAVE``) do not, so they are narrowed to the common
    nodes: a private node named alongside common ones is dropped from the
    set, and a policy that named nothing else fails with ``-EINVAL`` for an
    empty nodemask.  Narrowing rather than rejecting keeps ``numactl
    --interleave=all`` working once a device brings a private node online.
    A policy naming only common nodes is unaffected regardless of mode.

    The mempolicy component uses ``MPOL_F_PRIVATE`` at fault-time to select
    ``ZONELIST_PRIVATE`` and makes the node's memory available for allocation.
    It is otherwise an ordinary, relaxable mempolicy: an unsatisfiable request
    (an unmovable allocation on a movable-only private node) simply falls back.


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
   * - May contiguous allocation operate on the node?
     - ``N_MEMORY_CONTIG_ALLOC``
   * - May compaction operate on the node?
     - ``N_MEMORY_COMPACTION``
   * - May reclaim operate on the node?
     - ``N_MEMORY_RECLAIM``
   * - May demotion operate on the node?
     - ``N_MEMORY_DEMOTION``

Test ``N_MEMORY`` to report or account for memory, where a private node's
pages count like any other.  Where the question is whether an allocation
could have landed somewhere, or whether a nodemask already covers everything
an unbound allocation can reach, test ``N_MEMORY_COMMON``: ``N_MEMORY``
also holds the private nodes, which no unbound allocation can use.


cpuset interaction
==================

cpuset.mems governs every ``N_MEMORY`` node, private nodes included, exactly as
it governs common ones.  Adding a private node to a cgroup's
``cpuset.mems`` grants that cgroup use of it for allocation, user placement,
and kernel demotion; removing it revokes that use.

A private-only requested set is handled according to the cpuset mode.  Cgroup
v2 and ``cpuset_v2_mode`` retain the requested ``cpuset.mems`` value.  Its
effective mask keeps requested private nodes granted by the parent and adds the
parent's common nodes, without inheriting unrequested private nodes.  Legacy v1
has no effective-mems fallback, so setting a private-only mask, or attaching a
task to a cpuset with one, fails with ``-ENOSPC``.  If hot-unplug removes the
last common node from a legacy cpuset, its mask becomes empty and its tasks are
moved to an ancestor.

On a ``cpuset.mems`` change only the userland-NUMA subset (``N_MEMORY_USER_NUMA``
nodes: common nodes and opted-in private nodes) is positionally remapped or
migrated.  A device-owned (non-``USER_NUMA``) private node is never positionally
remapped and never migrated to or from: a cpuset change can drop it from a policy
but can never fold a policy onto one it never named.


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

Boot memory
-----------

Memory the boot path onlines never reaches that call - ``free_area_init()``
publishes it as system RAM before any driver runs - so a CXL window the BIOS
reports in E820 and the SRAT is never offered to dax/kmem.  It can be named
on the command line instead::

  private_node=<nid>[,reclaim][,compact][,contig][,demotion][,user]

The optional names follow the rules below and default to no features.  The node
carries the resulting mask from boot, so a driver cannot later claim it.
Unknown or repeated names and repeated declarations of one node are rejected.
``demotion`` requires ``reclaim``. A node is also refused if it owns CPUs or
the kernel image, or if Kexec Handover is enabled.

This is a development and bring-up interface for early systems and for
exercising private nodes on hardware that offers none.  Production private
memory should be provisioned by its owning driver.

Private-node capabilities
=========================

Private memory is excluded from opportunistic folio walkers, including KSM,
THP collapse, DAMON-native monitoring, filtering, statistics and migration,
and automatic NUMA balancing.  Those services treat a private-node folio like
a ``ZONE_DEVICE`` folio and have no private-node opt-in.  Madvise-backed DAMOS
actions delegate folio handling to the underlying MM service and follow that
service's eligibility rules.

The operations needed by the initial private-memory users are requested
explicitly through a feature bit in the ``features`` argument to
``node_features_register()``, which ``__add_memory_driver_managed()`` passes on
the driver's behalf:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Feature
     - Re-enables
   * - ``NODE_MEMORY_FEAT_COMPACTION``
     - direct, background, proactive and explicitly requested compaction
   * - ``NODE_MEMORY_FEAT_CONTIG_ALLOC``
     - contiguous allocation, including the isolation, migration and
       clean-page eviction needed to free a target range
   * - ``NODE_MEMORY_FEAT_RECLAIM``
     - reclaim of the node's folios, by the mm and by userspace
       ``MADV_COLD`` / ``PAGEOUT`` / ``FREE``
   * - ``NODE_MEMORY_FEAT_DEMOTION``
     - reclaim-driven tiering demotion onto the node (the node joins the
       demotion hierarchy)
   * - ``NODE_MEMORY_FEAT_USER_NUMA``
     - all userspace-directed placement and migration: ``mbind()`` /
       ``set_mempolicy()`` / home node, ``move_pages()`` /
       ``migrate_pages()`` to/from the node, and HugeTLB pool placement
       (every route into the pool names a node from userspace)

``NODE_MEMORY_FEAT_COMMON`` is what makes a node ordinary: a node with it
set holds every feature, and its absence is what makes a node private.

Dependencies between features are enforced once, by
``node_features_register()`` at hotplug, rather than by each user:

* ``DEMOTION`` requires ``RECLAIM`` because a demotion target needs a way to
  release the pages it accumulates.

Feature flags are expected to be stable at runtime.

Observability
=============

Userspace is given the two distinctions it can act on, as ordinary node-state
nodelists alongside ``has_memory`` and ``has_cpu``:

* ``/sys/devices/system/node/has_user_memory`` -- the nodes userspace may place
  memory on.  A memory node absent from this list cannot be named in a
  mempolicy or migration call.
* ``/sys/devices/system/node/has_common_memory`` -- the nodes an allocation
  reaches without naming them.  A memory node absent from this list only ever
  receives explicitly bound memory, so its capacity must not be counted towards
  what an unbound allocation can obtain.

A private node is one that is in ``has_memory`` but not in
``has_common_memory``; it may still be in ``has_user_memory``, which is the
difference between a device tier a task can bind to and one it cannot touch.

The full ``NODE_MEMORY_FEAT_*`` mask is not published anywhere: the bit
layout is kernel-internal and would otherwise become ABI, and nothing outside
the node that owns it needs the individual bits.  Whoever provisioned a node
already knows the capabilities it requested -- named features in
``private_node=`` or the mask passed by a driver to
``__add_memory_driver_managed()``.  An unsupported request is refused rather
than adjusted, so a node that came up private carries exactly what was
requested.

Otherwise a private node is reported like any other:

* ``/proc/<pid>/numa_maps`` -- per-node residency includes private nodes
* ``/proc/kcore`` -- private-node RAM appears in the kcore RAM map
* memcg per-node statistics account private-node memory.

Testing
========

The MM selftests discover private nodes that were provisioned before they
start.  A virtual machine can provide the required topology without a private
memory device by describing several memory-only NUMA nodes and marking them
on the kernel command line, for example::

  private_node=1,reclaim,compact,contig,demotion,user private_node=2,user \
  private_node=3,reclaim

With the feature definitions above, node 1 opts into every private-node
service, node 2 opts into USER_NUMA only, and node 3 opts into reclaim only.
At least one ordinary memory node must remain for fallback allocations.

The tests do not create, online or reconfigure nodes.  They skip cases for
which the running topology has no suitable node.  In particular, tests that
place anonymous memory on a private node need a node with USER_NUMA.

KTAP tests for excluding common MM services from private memory live in
``tools/testing/selftests/mm/`` and can be run with::

  ./run_vmtests.sh -t private_node
