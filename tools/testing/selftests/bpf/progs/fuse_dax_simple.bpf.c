// SPDX-License-Identifier: GPL-2.0
/*
 * BPF struct_ops for FUSE DAX simple (linear) extent resolution.
 *
 * iomap_setup reads per-file metadata from a BPF hashmap (populated by
 * the FUSE server at open time) and writes it into the per-inode meta_buf.
 * Device paths are read from a second hashmap and resolved via kfunc.
 *
 * iomap_begin reads meta_buf at fault time to resolve file offsets to
 * DAX device addresses. meta_buf format:
 *   struct dax_simple_meta_hdr
 *   n_extents * struct dax_simple_meta_ext
 */

#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <linux/errno.h>

char _license[] SEC("license") = "GPL";

/* kfunc declarations */
extern __u8 *bpf_fuse_dax_setup_get_meta(
	struct fuse_dax_fmap_parse_ctx *ctx,
	__u32 offset, const __u32 rdwr_buf_size) __ksym;
extern int bpf_fuse_dax_setup_add_device(
	struct fuse_dax_fmap_parse_ctx *ctx,
	__u32 dev_index,
	const char *path__buf, __u32 path_len__sz) __ksym;
extern const __u8 *bpf_fuse_dax_resolve_get_meta(
	struct fuse_dax_fmap_resolve_ctx *ctx,
	__u32 offset, const __u32 rdonly_buf_size) __ksym;

#define MAX_SIMPLE_EXTENTS	2048
#define FAMFS_BPF_MAX_EXTENTS	32
#define FAMFS_BPF_MAX_STRIPS	16

/* ---- famfs wire structs (shared with FUSE server) ---- */

enum fuse_famfs_file_type {
	FUSE_FAMFS_FILE_REG,
	FUSE_FAMFS_FILE_SUPERBLOCK,
	FUSE_FAMFS_FILE_LOG,
};

enum famfs_ext_type {
	FUSE_FAMFS_EXT_SIMPLE = 0,
	FUSE_FAMFS_EXT_INTERLEAVE = 1,
};

struct famfs_meta_simple_ext_bpf {
	__u64 dev_index;
	__u64 ext_offset;
	__u64 ext_len;
};

struct famfs_meta_interleaved_ext_bpf {
	__u64 fie_nstrips;
	__u64 fie_chunk_size;
	__u64 fie_nbytes;
	struct famfs_meta_simple_ext_bpf ie_strips[FAMFS_BPF_MAX_STRIPS];
};

struct famfs_file_meta {
	__u8   error;
	__u32  file_type;
	__u64  file_size;
	__u32  fm_extent_type;
	__u64  dev_bitmap;
	union {
		struct {
			__u64 fm_nextents;
			struct famfs_meta_simple_ext_bpf se[FAMFS_BPF_MAX_EXTENTS];
		};
		struct {
			__u64 fm_niext;
			struct famfs_meta_interleaved_ext_bpf ie[FAMFS_BPF_MAX_EXTENTS];
		};
	};
};

struct famfs_dev_info {
	char path[256];
};

/* ---- BPF hashmaps (populated by FUSE server, read by iomap_setup) ---- */

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 32768);
	__type(key, __u64);                    /* nodeid */
	__type(value, struct famfs_file_meta);
} famfs_meta_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 24);
	__type(key, __u32);                    /* dev_index */
	__type(value, struct famfs_dev_info);
} famfs_dev_map SEC(".maps");

/* ---- meta_buf format (same as before, consumed by iomap_begin) ---- */

struct dax_simple_meta_hdr {
	__u32 n_extents;
	__u32 reserved;
};

struct dax_simple_meta_ext {
	__u32 dev_index;
	__u32 reserved;
	__u64 offset;
	__u64 len;
};

/* ---- iomap_setup: replaces dax_fmap_parse ---- */

SEC("struct_ops.s/iomap_setup")
int BPF_PROG(famfs_iomap_setup, struct fuse_dax_fmap_parse_ctx *pctx)
{
	struct famfs_file_meta *fmeta;
	struct dax_simple_meta_hdr *mhdr;
	__u64 nodeid = pctx->nodeid;
	__u32 n_extents;

	fmeta = bpf_map_lookup_elem(&famfs_meta_map, &nodeid);
	if (!fmeta)
		return -ENOENT;

	if (fmeta->error)
		return -EIO;

	if (fmeta->fm_extent_type != FUSE_FAMFS_EXT_SIMPLE)
		return -EOPNOTSUPP;

	n_extents = fmeta->fm_nextents;
	if (n_extents > MAX_SIMPLE_EXTENTS)
		return -EINVAL;

	mhdr = (struct dax_simple_meta_hdr *)
		bpf_fuse_dax_setup_get_meta(pctx, 0, sizeof(*mhdr));
	if (!mhdr)
		return -EINVAL;

	mhdr->n_extents = n_extents;
	mhdr->reserved = 0;

	for (__u32 i = 0; i < FAMFS_BPF_MAX_EXTENTS && i < n_extents; i++) {
		struct dax_simple_meta_ext *mext;
		__u32 meta_off = sizeof(*mhdr) + i * sizeof(*mext);
		__u32 dev_idx;

		mext = (struct dax_simple_meta_ext *)
			bpf_fuse_dax_setup_get_meta(pctx, meta_off,
						    sizeof(*mext));
		if (!mext)
			return -EINVAL;

		dev_idx = (__u32)fmeta->se[i].dev_index;

		mext->dev_index = dev_idx;
		mext->reserved = 0;
		mext->offset = fmeta->se[i].ext_offset;
		mext->len = fmeta->se[i].ext_len;

		pctx->dev_bitmap |= (1ULL << dev_idx);
	}

	/* Resolve each device referenced in dev_bitmap */
	for (__u32 d = 0; d < 24; d++) {
		struct famfs_dev_info *dinfo;
		__u32 idx = d;
		int rc;

		if (!(pctx->dev_bitmap & (1ULL << d)))
			continue;

		dinfo = bpf_map_lookup_elem(&famfs_dev_map, &idx);
		if (!dinfo)
			return -ENOENT;

		rc = bpf_fuse_dax_setup_add_device(pctx, idx,
						    dinfo->path, 255);
		if (rc)
			return rc;
	}

	return 0;
}

/* ---- iomap_begin: UNCHANGED from original ---- */

SEC("struct_ops/iomap_begin")
int BPF_PROG(dax_simple_iomap_begin, struct fuse_dax_fmap_resolve_ctx *rctx,
	     struct fuse_iomap_io *io)
{
	const struct dax_simple_meta_hdr *mhdr;
	__u64 local_offset = rctx->file_offset;
	__u32 n_extents;

	mhdr = (const struct dax_simple_meta_hdr *)
		bpf_fuse_dax_resolve_get_meta(rctx, 0, sizeof(*mhdr));
	if (!mhdr)
		return -EIO;

	n_extents = mhdr->n_extents;
	if (n_extents > MAX_SIMPLE_EXTENTS)
		return -EIO;

	for (__u32 i = 0; i < MAX_SIMPLE_EXTENTS && i < n_extents; i++) {
		const struct dax_simple_meta_ext *mext;
		__u32 meta_off = sizeof(*mhdr) + i * sizeof(*mext);

		mext = (const struct dax_simple_meta_ext *)
			bpf_fuse_dax_resolve_get_meta(rctx, meta_off,
						      sizeof(*mext));
		if (!mext)
			return -EIO;

		if (local_offset < mext->len) {
			__u64 remaining = mext->len - local_offset;

			io->dev_index = mext->dev_index;
			io->addr = mext->offset + local_offset;
			io->length = remaining < rctx->length
				? remaining : rctx->length;
			io->offset = rctx->file_offset;
			io->type = 0; /* IOMAP_MAPPED filled by kernel */
			io->flags = 0;
			return 0;
		}
		local_offset -= mext->len;
	}

	return -EIO;
}

/* ---- struct_ops definition ---- */

SEC(".struct_ops.link")
struct fuse_dax_fmap_ops dax_simple_ops = {
	.name = "dax_simple",
	.meta_size = sizeof(struct dax_simple_meta_hdr) +
		     MAX_SIMPLE_EXTENTS * sizeof(struct dax_simple_meta_ext),
	.iomap_setup = (void *)famfs_iomap_setup,
	.iomap_begin = (void *)dax_simple_iomap_begin,
};
