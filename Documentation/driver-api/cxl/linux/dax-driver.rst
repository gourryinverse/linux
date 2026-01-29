.. SPDX-License-Identifier: GPL-2.0

====================
DAX Driver Operation
====================
The `Direct Access Device` driver was originally designed to provide a
memory-like access mechanism to memory-like block-devices.  It was
extended to support CXL Memory Devices, which provide user-configured
memory devices.

The CXL subsystem depends on the DAX subsystem to either:

- Generate a file-like interface to userland via :code:`/dev/daxN.Y`, or
- Engage the memory-hotplug interface to add CXL memory to page allocator.

The DAX subsystem exposes this ability through the `cxl_dax_region` driver.
A `dax_region` provides the translation between a CXL `memory_region` and
a `DAX Device`.

CXL DAX Region Drivers
======================
CXL provides multiple drivers for creating DAX regions, each suited for
different use cases:

cxl_devdax_region
-----------------
The :code:`cxl_devdax_region` driver creates a dax_region configured for
device_dax mode.  When a CXL RAM region is bound to this driver, the
resulting DAX device provides direct userspace access via :code:`/dev/daxN.Y`.

Device hierarchy::

  regionX -> dax_regionX -> daxX.Y

This is the simplest path for applications that want to manage CXL memory
directly from userspace.

cxl_dax_kmem_region
-------------------
For kmem mode, CXL provides a two-stage binding process that allows users
to configure memory hotplug policy before memory is added to the system.

The :code:`cxl_dax_kmem_region` driver then binds a sysram_region
device and creates a dax_region configured for kmem mode.

The :code:`online_type` policy will be passed from sysram_region to
the dax kmem driver for use when hotplugging the memory.

DAX Device
==========
A `DAX Device` is a file-like interface exposed in :code:`/dev/daxN.Y`. A
memory region exposed via dax device can be accessed via userland software
via the :code:`mmap()` system-call.  The result is direct mappings to the
CXL capacity in the task's page tables.

Users wishing to manually handle allocation of CXL memory should use this
interface.

kmem conversion
===============
The :code:`dax_kmem` driver converts a `DAX Device` into a series of `hotplug
memory blocks` managed by :code:`kernel/memory-hotplug.c`.  This capacity
will be exposed to the kernel page allocator in the user-selected memory
zone.

The :code:`memmap_on_memory` setting (both global and DAX device local)
dictates where the kernell will allocate the :code:`struct folio` descriptors
for this memory will come from.  If :code:`memmap_on_memory` is set, memory
hotplug will set aside a portion of the memory block capacity to allocate
folios. If unset, the memory is allocated via a normal :code:`GFP_KERNEL`
allocation - and as a result will most likely land on the local NUM node of the
CPU executing the hotplug operation.
