/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FUSE_DAX_FMAP_OPS_H
#define _LINUX_FUSE_DAX_FMAP_OPS_H

#include <linux/types.h>

#define FUSE_DAX_FMAP_OPS_NAME_LEN 16
#define FUSE_DAX_FMAP_META_MAX (64 * 1024)

struct fuse_conn;

struct fuse_iomap_io {
	__u64 offset;
	__u64 length;
	__u64 addr;
	__u16 type;
	__u16 flags;
	__u32 dev_index;
};

/*
 * Context passed to the BPF iomap_setup() callback at file open time.
 * Process context, sleepable. Called once per file open.
 *
 * The meta_buf is NOT directly accessible as a pointer field. BPF
 * programs use kfuncs to get bounded pointers:
 *   bpf_fuse_dax_setup_get_meta() - write the metadata buffer
 *
 * Scalar OUT fields are written directly by the BPF program.
 */
struct fuse_dax_fmap_parse_ctx {
	__u64 nodeid;		/* IN: FUSE node ID for hashmap lookup */
	__u32 meta_buf_size;
	__u64 dev_bitmap;		/* OUT: bitmap of referenced dev indices */
};

/*
 * Context passed to the BPF iomap_begin() callback at iomap_begin time.
 * Non-sleepable, hot path. Called on every page fault / IO.
 *
 * All fields are INPUT-only. Output goes to struct fuse_iomap_io.
 *
 * The meta_buf is accessed via kfunc:
 *   bpf_fuse_dax_resolve_get_meta() - read the metadata buffer
 */
struct fuse_dax_fmap_resolve_ctx {
	__u32 meta_buf_size;
	__u64 file_offset;
	__u64 length;
};

/*
 * BPF struct_ops for FUSE DAX fmap extent resolution.
 *
 * iomap_setup() is called at file open in process context (sleepable).
 *   Reads from BPF hashmap (populated by FUSE server), writes meta_buf.
 * iomap_begin() is called at iomap_begin in fault context (non-sleepable).
 *   Reads meta_buf cached on the fuse_inode.
 */
struct fuse_dax_fmap_ops {
	char name[FUSE_DAX_FMAP_OPS_NAME_LEN];
	__u32 meta_size;
	int (*iomap_setup)(struct fuse_dax_fmap_parse_ctx *ctx);
	int (*iomap_begin)(struct fuse_dax_fmap_resolve_ctx *ctx,
			   struct fuse_iomap_io *io);
};

/*
 * Kernel-internal context wrappers. The kern structs embed the
 * user-visible ctx plus the actual buffer pointers. BPF programs
 * see only the user-visible portion; kfuncs bridge to the buffers
 * via container_of().
 */
struct fuse_dax_fmap_parse_ctx_kern {
	struct fuse_dax_fmap_parse_ctx ctx;
	void *meta_buf;
	struct fuse_conn *fc;
};

struct fuse_dax_fmap_resolve_ctx_kern {
	struct fuse_dax_fmap_resolve_ctx ctx;
	const void *meta_buf;
};

#if IS_ENABLED(CONFIG_FUSE_DAX_FMAP_BPF)
struct bpf_link;
struct fuse_dax_fmap_ops *fuse_dax_fmap_ops_find(const char *name,
						 struct bpf_link **linkp);
#else
static inline
struct fuse_dax_fmap_ops *fuse_dax_fmap_ops_find(const char *name,
						 struct bpf_link **linkp)
{
	return NULL;
}
#endif

#endif /* _LINUX_FUSE_DAX_FMAP_OPS_H */
