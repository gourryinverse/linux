.. SPDX-License-Identifier: GPL-2.0

.. _famfs_index:

==================================================================
FUSE DAX fmap: Direct memory access for FUSE filesystems
==================================================================

- Copyright (C) 2024-2026 Micron Technology, Inc.

Introduction
============
Compute Express Link (CXL) provides a mechanism for disaggregated or
fabric-attached memory (FAM). This creates opportunities for data sharing;
clustered apps that would otherwise have to shard or replicate data can
share one copy in disaggregated memory.

The FUSE DAX fmap infrastructure provides a mechanism for FUSE
filesystems (such as famfs) to map files directly onto devdax memory,
allowing read, write, and mmap access with no page cache involvement.
The CPU cache is loaded directly from the backing memory, providing
direct access (DAX) semantics.

Architecture
============

FUSE DAX fmap uses a BPF struct_ops architecture for extent resolution.
The kernel understands no extent formats natively — all parsing and
resolution is handled by BPF programs that are registered as struct_ops
implementations.

Metadata Delivery
-----------------

File mapping metadata is delivered via BPF hashmaps populated by the FUSE
server. At file open time, the server pushes per-file extent metadata into
a BPF hashmap (keyed by FUSE node ID). The kernel's BPF ``iomap_setup()``
callback reads this hashmap entry and populates an opaque per-inode
metadata buffer.

Device information is similarly pushed into a separate BPF hashmap (keyed
by device index) at mount time. The BPF ``iomap_setup()`` callback
resolves devices by reading the device map and calling the
``bpf_fuse_dax_setup_add_device()`` kfunc.

The ``meta_size`` for metadata buffer allocation comes from the
``fuse_dax_fmap_ops.meta_size`` field set by the BPF program at
registration time.

FUSE_INIT Negotiation
---------------------

The BPF struct_ops program name is communicated during FUSE_INIT. The
server sets the ``ops_name`` field in ``fuse_init_out`` to identify which
BPF program should interpret its blobs (e.g., ``"dax_simple"``).

On INIT reply, the kernel looks up the named BPF struct_ops and pins
both the ops pointer and its ``bpf_link`` on ``fuse_conn``. If the
named BPF program is not loaded, DAX fmap is not negotiated and the
mount proceeds without DAX support.

The BPF program cannot be unloaded while the filesystem is mounted —
the ``bpf_link`` reference on ``fuse_conn`` prevents it. The link is
released during ``fuse_dax_fmap_teardown()`` at unmount.

BPF struct_ops Interface
------------------------

BPF programs implement the ``fuse_dax_fmap_ops`` interface::

    struct fuse_dax_fmap_ops {
        char name[16];
        __u32 meta_size;
        int (*iomap_setup)(struct fuse_dax_fmap_parse_ctx *ctx);
        int (*iomap_begin)(struct fuse_dax_fmap_resolve_ctx *ctx,
                           struct fuse_iomap_io *io);
    };

**iomap_setup()** — called once per file open in process context
(sleepable). Reads per-file metadata from a BPF hashmap (populated by
the FUSE server), populates a metadata buffer for later use by
iomap_begin(), resolves devices, and sets a device bitmap. Uses kfuncs:

- ``bpf_fuse_dax_setup_get_meta(ctx, offset, size)`` — write metadata buffer
- ``bpf_fuse_dax_setup_add_device(ctx, dev_index, path, path_len)`` — resolve DAX device

Sets output field on the parse context: ``dev_bitmap``.

**iomap_begin()** — called on every page fault and I/O in fault context
(non-sleepable, hot path). Translates a file offset to a physical
device offset using the metadata buffer populated by iomap_setup().
Uses:

- ``bpf_fuse_dax_resolve_get_meta(ctx, offset, size)`` — read metadata buffer

Writes output to ``struct fuse_iomap_io``::

    struct fuse_iomap_io {
        __u64 offset;    /* file offset */
        __u64 length;    /* mapped length */
        __u64 addr;      /* physical address on device */
        __u16 type;      /* IOMAP_MAPPED, etc. */
        __u16 flags;     /* IOMAP_F_* flags */
        __u32 dev_index; /* index into connection device list */
    };

Data Flow
---------

File open (cold path)::

    allocate metadata buffer (ops->meta_size bytes)
    → BPF ops->iomap_setup(parse_ctx)
      → read BPF hashmap for nodeid
      → populate meta_buf via kfunc
      → resolve devices via bpf_fuse_dax_setup_add_device()
    → store {meta, meta_size} on inode
    → set S_DAX flag

Page fault / I/O (hot path)::

    iomap_begin(inode, offset, length, ...)
    → BPF ops->iomap_begin(resolve_ctx, &io)
    → validate io.dev_index, look up daxdev
    → fill iomap struct with {addr, length, dax_dev}

Device Resolution
-----------------

Device resolution is handled directly by BPF ``iomap_setup()`` programs.
The FUSE server populates a BPF hashmap with device paths (keyed by
device index) at mount time. During ``iomap_setup()``, the BPF program
reads device paths from the hashmap and calls
``bpf_fuse_dax_setup_add_device(ctx, dev_index, path, path_len)`` to
resolve each device. This kfunc calls ``kern_path()`` + ``dax_dev_get()``
+ ``fs_dax_get()`` internally. Resolved devices are cached on the
``fuse_conn`` for the lifetime of the connection.

Reference BPF Programs
======================

Two reference BPF struct_ops implementations are provided:

**dax_simple** (``tools/testing/selftests/bpf/progs/fuse_dax_simple.bpf.c``)
  Linear extent list. Each extent is a ``(dev_index, offset, length)``
  tuple. Resolution is O(n) in the number of extents.

**dax_interleave** (``tools/testing/selftests/bpf/progs/fuse_dax_interleave.bpf.c``)
  Striped (RAID-0 style) extents. Data is distributed in round-robin
  fashion across strips, where each strip is a contiguous allocation on
  a dax device. Resolution is O(1) within a single interleaved extent
  via chunk/strip/stripe arithmetic.

Interleaved Extent Layout
--------------------------

An interleaved extent stripes data across a collection of strips::

    chunk_size = 2 MiB, nstrips = 4, nbytes = 24 MiB

    ┌────────────┐────────────┐────────────┐────────────┐
    │Chunk 0     │Chunk 1     │Chunk 2     │Chunk 3     │
    │Strip 0     │Strip 1     │Strip 2     │Strip 3     │
    │Stripe 0    │Stripe 0    │Stripe 0    │Stripe 0    │
    └────────────┘────────────┘────────────┘────────────┘
    │Chunk 4     │Chunk 5     │Chunk 6     │Chunk 7     │
    │Strip 0     │Strip 1     │Strip 2     │Strip 3     │
    │Stripe 1    │Stripe 1    │Stripe 1    │Stripe 1    │
    └────────────┘────────────┘────────────┘────────────┘
    │Chunk 8     │Chunk 9     │Chunk 10    │Chunk 11    │
    │Strip 0     │Strip 1     │Strip 2     │Strip 3     │
    │Stripe 2    │Stripe 2    │Stripe 2    │Stripe 2    │
    └────────────┘────────────┘────────────┘────────────┘

Resolution arithmetic for offset within an interleaved extent::

    chunk_num    = offset / chunk_size
    chunk_offset = offset % chunk_size
    strip_num    = chunk_num % nstrips
    stripe_num   = chunk_num / nstrips
    strip_offset = chunk_offset + stripe_num * chunk_size
    device_offset = strip[strip_num].offset + strip_offset

Memory Error Handling
=====================

Possible memory errors include timeouts, poison, and unexpected
reconfiguration of an underlying dax device. In all of these cases, the
kernel receives a call from the devdax layer via the DAX holder
``notify_failure()`` callback. If any memory errors have been detected,
access to the affected daxdev is disabled to avoid further errors or
corruption.

Configuration
=============

The FUSE DAX fmap infrastructure requires::

    CONFIG_FUSE_DAX_FMAP=y    # Core fmap support
    CONFIG_FUSE_DAX_FMAP_BPF=y  # BPF struct_ops for extent resolution
    CONFIG_DEV_DAX_FSDEV=y    # fs-dax compatible devdax driver

References
==========

- Famfs user space repository and documentation:
  https://github.com/cxl-micron-reskit/famfs
