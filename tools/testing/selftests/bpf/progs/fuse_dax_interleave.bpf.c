// SPDX-License-Identifier: GPL-2.0
/*
 * BPF struct_ops for FUSE DAX interleaved (striped) extent resolution.
 *
 * iomap_setup reads per-file metadata from a BPF hashmap (populated by
 * the FUSE server at open time) and writes it into the per-inode meta_buf.
 *
 * meta_buf format:
 *   struct dax_ileave_meta_hdr
 *   n_iexts * struct dax_ileave_meta_iext
 *   total_strips * struct dax_ileave_meta_strip
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

#define MAX_IEXTS		16
#define MAX_STRIPS_PER_IEXT	32
#define MAX_TOTAL_STRIPS	512
#define FAMFS_BPF_MAX_EXTENTS	32
#define FAMFS_BPF_MAX_STRIPS	16

/* ---- famfs wire structs (shared with FUSE server) ---- */

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

/* ---- BPF hashmaps ---- */

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 32768);
	__type(key, __u64);
	__type(value, struct famfs_file_meta);
} famfs_meta_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 24);
	__type(key, __u32);
	__type(value, struct famfs_dev_info);
} famfs_dev_map SEC(".maps");

/* ---- meta_buf format ---- */

struct dax_ileave_meta_hdr {
	__u32 n_iexts;
	__u32 reserved;
	__u64 file_size;
};

struct dax_ileave_meta_iext {
	__u64 chunk_size;
	__u64 nstrips;
	__u64 nbytes;
	__u32 strip_base;
	__u32 reserved;
};

struct dax_ileave_meta_strip {
	__u64 dev_index;
	__u64 offset;
	__u64 len;
};

/* ---- iomap_setup: replaces dax_fmap_parse ---- */

SEC("struct_ops.s/iomap_setup")
int BPF_PROG(dax_ileave_setup, struct fuse_dax_fmap_parse_ctx *pctx)
{
	struct famfs_file_meta *fmeta;
	struct dax_ileave_meta_hdr *mhdr;
	__u64 nodeid = pctx->nodeid;
	__u32 n_iexts;
	__u32 strip_idx = 0;
	__u32 iexts_meta_off;

	fmeta = bpf_map_lookup_elem(&famfs_meta_map, &nodeid);
	if (!fmeta)
		return -ENOENT;

	if (fmeta->error)
		return -EIO;

	if (fmeta->fm_extent_type != FUSE_FAMFS_EXT_INTERLEAVE)
		return -EOPNOTSUPP;

	n_iexts = (__u32)fmeta->fm_niext;
	if (n_iexts > MAX_IEXTS)
		return -EINVAL;

	mhdr = (struct dax_ileave_meta_hdr *)
		bpf_fuse_dax_setup_get_meta(pctx, 0, sizeof(*mhdr));
	if (!mhdr)
		return -EINVAL;

	mhdr->n_iexts = n_iexts;
	mhdr->reserved = 0;
	mhdr->file_size = fmeta->file_size;

	iexts_meta_off = sizeof(*mhdr);

	for (__u32 i = 0; i < MAX_IEXTS && i < n_iexts; i++) {
		struct dax_ileave_meta_iext *miext;
		__u32 meta_off = iexts_meta_off + i * sizeof(*miext);
		__u32 nstrips;

		if (i >= FAMFS_BPF_MAX_EXTENTS)
			break;

		miext = (struct dax_ileave_meta_iext *)
			bpf_fuse_dax_setup_get_meta(pctx, meta_off,
						    sizeof(*miext));
		if (!miext)
			return -EINVAL;

		nstrips = (__u32)fmeta->ie[i].fie_nstrips;
		if (nstrips > MAX_STRIPS_PER_IEXT || nstrips == 0)
			return -EINVAL;

		miext->chunk_size = fmeta->ie[i].fie_chunk_size;
		miext->nstrips = nstrips;
		miext->nbytes = fmeta->ie[i].fie_nbytes;
		miext->strip_base = strip_idx;
		miext->reserved = 0;

		for (__u32 j = 0; j < MAX_STRIPS_PER_IEXT && j < nstrips; j++) {
			struct dax_ileave_meta_strip *mstrip;
			__u32 strips_start = iexts_meta_off +
				n_iexts * sizeof(*miext);
			__u32 soff = strips_start +
				(strip_idx + j) * sizeof(*mstrip);
			__u32 dev_idx;

			if (j >= FAMFS_BPF_MAX_STRIPS)
				break;
			if (strip_idx + j >= MAX_TOTAL_STRIPS)
				return -EINVAL;

			mstrip = (struct dax_ileave_meta_strip *)
				bpf_fuse_dax_setup_get_meta(pctx, soff,
							    sizeof(*mstrip));
			if (!mstrip)
				return -EINVAL;

			dev_idx = (__u32)fmeta->ie[i].ie_strips[j].dev_index;
			mstrip->dev_index = dev_idx;
			mstrip->offset = fmeta->ie[i].ie_strips[j].ext_offset;
			mstrip->len = fmeta->ie[i].ie_strips[j].ext_len;

			pctx->dev_bitmap |= (1ULL << dev_idx);
		}

		strip_idx += nstrips;
	}

	/* Resolve devices */
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

/* ---- iomap_begin: reads file_size from meta_buf instead of resolve_ctx ---- */

SEC("struct_ops/iomap_begin")
int BPF_PROG(dax_ileave_iomap_begin, struct fuse_dax_fmap_resolve_ctx *rctx,
	     struct fuse_iomap_io *io)
{
	const struct dax_ileave_meta_hdr *mhdr;
	__u64 local_offset = rctx->file_offset;
	__u64 file_size;
	__u32 n_iexts;

	mhdr = (const struct dax_ileave_meta_hdr *)
		bpf_fuse_dax_resolve_get_meta(rctx, 0, sizeof(*mhdr));
	if (!mhdr)
		return -EIO;

	n_iexts = mhdr->n_iexts;
	file_size = mhdr->file_size;
	if (n_iexts > MAX_IEXTS)
		return -EIO;

	for (__u32 i = 0; i < MAX_IEXTS && i < n_iexts; i++) {
		const struct dax_ileave_meta_iext *miext;
		__u32 iext_off = sizeof(*mhdr) + i * sizeof(*miext);
		__u64 chunk_size, nstrips, ext_size;

		miext = (const struct dax_ileave_meta_iext *)
			bpf_fuse_dax_resolve_get_meta(rctx, iext_off,
						      sizeof(*miext));
		if (!miext)
			return -EIO;

		chunk_size = miext->chunk_size;
		nstrips = miext->nstrips;
		ext_size = miext->nbytes;

		if (chunk_size == 0 || nstrips == 0)
			return -EIO;

		if (ext_size > file_size)
			ext_size = file_size;

		if (local_offset < ext_size) {
			const struct dax_ileave_meta_strip *mstrip;
			__u64 chunk_num = local_offset / chunk_size;
			__u64 chunk_offset = local_offset % chunk_size;
			__u64 chunk_remainder = chunk_size - chunk_offset;
			__u64 strip_num = chunk_num % nstrips;
			__u64 stripe_num = chunk_num / nstrips;
			__u64 strip_offset = chunk_offset +
				stripe_num * chunk_size;
			__u32 strip_idx = miext->strip_base + (__u32)strip_num;
			__u32 strips_start = sizeof(*mhdr) +
				n_iexts * sizeof(*miext);
			__u32 soff = strips_start +
				strip_idx * sizeof(*mstrip);

			mstrip = (const struct dax_ileave_meta_strip *)
				bpf_fuse_dax_resolve_get_meta(rctx, soff,
							      sizeof(*mstrip));
			if (!mstrip)
				return -EIO;

			io->dev_index = (__u32)mstrip->dev_index;
			io->addr = mstrip->offset + strip_offset;
			io->length = chunk_remainder < rctx->length
				? chunk_remainder : rctx->length;
			io->offset = rctx->file_offset;
			io->type = 0;
			io->flags = 0;
			return 0;
		}

		local_offset -= ext_size;
	}

	return -EIO;
}

SEC(".struct_ops.link")
struct fuse_dax_fmap_ops dax_ileave_ops = {
	.name = "dax_interleave",
	.meta_size = sizeof(struct dax_ileave_meta_hdr) +
		     MAX_IEXTS * sizeof(struct dax_ileave_meta_iext) +
		     MAX_TOTAL_STRIPS * sizeof(struct dax_ileave_meta_strip),
	.iomap_setup = (void *)dax_ileave_setup,
	.iomap_begin = (void *)dax_ileave_iomap_begin,
};
