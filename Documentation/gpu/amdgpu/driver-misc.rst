================================
 Misc AMDGPU driver information
================================

GPU Product Information
=======================

Information about the GPU can be obtained on certain cards
via sysfs

product_name
------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_fru_eeprom.c
   :doc: product_name

product_number
--------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_fru_eeprom.c
   :doc: product_number

serial_number
-------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_fru_eeprom.c
   :doc: serial_number

fru_id
-------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_fru_eeprom.c
   :doc: fru_id

manufacturer
-------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_fru_eeprom.c
   :doc: manufacturer

unique_id
---------

.. kernel-doc:: drivers/gpu/drm/amd/pm/amdgpu_pm.c
   :doc: unique_id

board_info
----------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: board_info

GPU Memory Usage Information
============================

Various memory accounting can be accessed via sysfs

mem_info_vram_total
-------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c
   :doc: mem_info_vram_total

mem_info_vram_used
------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c
   :doc: mem_info_vram_used

mem_info_vis_vram_total
-----------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c
   :doc: mem_info_vis_vram_total

mem_info_vis_vram_used
----------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c
   :doc: mem_info_vis_vram_used

mem_info_gtt_total
------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_gtt_mgr.c
   :doc: mem_info_gtt_total

mem_info_gtt_used
-----------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_gtt_mgr.c
   :doc: mem_info_gtt_used

PCIe Accounting Information
===========================

pcie_bw
-------

.. kernel-doc:: drivers/gpu/drm/amd/pm/amdgpu_pm.c
   :doc: pcie_bw

pcie_replay_count
-----------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: pcie_replay_count

GPU SmartShift Information
==========================

GPU SmartShift information via sysfs

smartshift_apu_power
--------------------

.. kernel-doc:: drivers/gpu/drm/amd/pm/amdgpu_pm.c
   :doc: smartshift_apu_power

smartshift_dgpu_power
---------------------

.. kernel-doc:: drivers/gpu/drm/amd/pm/amdgpu_pm.c
   :doc: smartshift_dgpu_power

smartshift_bias
---------------

.. kernel-doc:: drivers/gpu/drm/amd/pm/amdgpu_pm.c
   :doc: smartshift_bias

UMA Carveout
============

Some versions of Atom ROM expose available options for the VRAM carveout sizes,
and allow changes to the carveout size via the ATCS function code 0xA on supported
BIOS implementations.

For those platforms, users can use the following files under uma/ to set the
carveout size, in a way similar to what Windows users can do in the "Tuning"
tab in AMD Adrenalin.

Note that for BIOS implementations that don't support this, these files will not
be created at all.

uma/carveout_options
--------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: uma/carveout_options

uma/carveout
--------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: uma/carveout

Reversible UMA memory donation
==============================

On supported APUs, part of the firmware-reserved UMA carveout can be moved
between GPU ownership and Linux System RAM at runtime.  The complete carveout
starts under GPU ownership, and no ownership transfer occurs until a nonzero
target is written to the donated-size sysfs file.  Donated memory is added as
driver-managed System RAM on the physical-address-selected NUMA node and is
explicitly onlined into ``ZONE_MOVABLE``; a private NUMA node is not required.

The initial implementation is limited to x86-64 kernels with memory hotplug
and hot-remove.  It excludes application APUs, passthrough devices,
CPU-connected XGMI devices, systems with host memory encryption, devices using
AMDGPU runtime power management, and devices without a CPU-visible aperture.
These are conservative implementation filters, not a firmware capability
contract for reclaimable UMA.  The raw-offset ``debugfs/amdgpu_vram`` file is
also omitted because it is not constrained by TTM ownership and could address
donated blocks directly.

The unit of every transaction is the memory-hotplug block size reported by the
running kernel.  The driver does not assume a fixed block size.  It advertises
the largest eligible block-aligned VRAM interval found at initialization, and
donation grows from the high end of that interval.  Movable GPU buffer objects
are evicted as needed, while pinned or driver-reserved occupants make a
transaction fail safely.

Returning memory first migrates one block's pages, takes that block offline,
and removes it from Linux.  Multi-block writes commit one block at a time.  If
a later block cannot transition, the write returns an error and the reported
donated size reflects the completed prefix.  A failed cache-attribute change
keeps the exact GPU guard and reports the block as quarantined until a later
write repairs it toward the requested ownership.

AMDGPU retains authority over the online state of its donated memory blocks.
Their generic ``memoryX/state`` files remain readable for observation, but a
direct attempt to take an online donated block offline is rejected with
``EBUSY``.  No ownership-changing state transition is permitted through the
generic interface; it must be requested through ``uma/donated_memory_bytes``
so GPU exclusion, CPU mapping attributes, and memory hotplug remain one
transaction.

System suspend and hibernation preparation return all donated blocks before
device power transitions begin.  The sleep transition is vetoed if return is
blocked, and the prior target is requested again after resume.  Runtime GPU
recovery uses the same exclusion: it returns donated memory before reset and
redonates only after successful recovery.  PCI error recovery also returns the
memory before a slot reset and restores the prior target after resume.  Device
removal likewise needs all donated memory to be returnable.  Teardown waits
rather than dismantling the GPU memory manager while System RAM remains live,
so unbind or shutdown can block until a pin is released.  Users should write
``0`` before unbinding or physically removing a device.

uma/donated_memory_bytes
------------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: uma/donated_memory_bytes

uma/quarantined_memory_bytes
----------------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: uma/quarantined_memory_bytes

uma/donation_block_size_bytes
-----------------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: uma/donation_block_size_bytes

uma/donatable_memory_bytes
--------------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: uma/donatable_memory_bytes

uma/donation_range_start
------------------------

.. kernel-doc:: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
   :doc: uma/donation_range_start
