// SPDX-License-Identifier: GPL-2.0-only
/*
 * BPF struct_ops for FUSE DAX fmap extent resolution.
 *
 * Copyright 2026 Gregory Price
 *
 * Modeled after drivers/hid/bpf/hid_bpf_struct_ops.c
 */

#include <linux/bpf_verifier.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/filter.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fuse_dax_fmap_ops.h>

#include "fuse_dax_fmap.h"
#include "fuse_i.h"

static struct btf *fuse_dax_fmap_ops_btf;

static int fuse_dax_fmap_ops_init(struct btf *btf)
{
	fuse_dax_fmap_ops_btf = btf;
	return 0;
}

static bool fuse_dax_fmap_ops_is_valid_access(int off, int size,
					      enum bpf_access_type type,
					      const struct bpf_prog *prog,
					      struct bpf_insn_access_aux *info)
{
	return bpf_tracing_btf_ctx_access(off, size, type, prog, info);
}

static int fuse_dax_fmap_ops_check_member(const struct btf_type *t,
					  const struct btf_member *member,
					  const struct bpf_prog *prog)
{
	u32 moff = __btf_member_bit_offset(t, member) / 8;

	switch (moff) {
	case offsetof(struct fuse_dax_fmap_ops, iomap_setup):
		break;
	default:
		if (prog->sleepable)
			return -EINVAL;
	}

	return 0;
}

static int fuse_dax_fmap_ops_btf_struct_access(struct bpf_verifier_log *log,
					       const struct bpf_reg_state *reg,
					       int off, int size)
{
	const struct btf_type *t;
	s32 type_id;

	t = btf_type_by_id(reg->btf, reg->btf_id);

	type_id = btf_find_by_name_kind(reg->btf, "fuse_dax_fmap_parse_ctx",
					BTF_KIND_STRUCT);
	if (type_id >= 0 && t == btf_type_by_id(reg->btf, type_id)) {
		if (off >= offsetof(struct fuse_dax_fmap_parse_ctx, dev_bitmap) &&
		    off + size <= offsetofend(struct fuse_dax_fmap_parse_ctx, dev_bitmap))
			return NOT_INIT;
		goto read_only;
	}

	type_id = btf_find_by_name_kind(reg->btf, "fuse_dax_fmap_resolve_ctx",
					BTF_KIND_STRUCT);
	if (type_id >= 0 && t == btf_type_by_id(reg->btf, type_id))
		goto read_only;

	type_id = btf_find_by_name_kind(reg->btf, "fuse_iomap_io",
					BTF_KIND_STRUCT);
	if (type_id >= 0 && t == btf_type_by_id(reg->btf, type_id))
		return NOT_INIT;

	bpf_log(log, "write access to this struct is not supported\n");
	return -EACCES;

read_only:
	bpf_log(log, "write access at off %d with size %d on read-only part\n",
		off, size);
	return -EACCES;
}

static const struct bpf_verifier_ops fuse_dax_fmap_verifier_ops = {
	.get_func_proto = bpf_base_func_proto,
	.is_valid_access = fuse_dax_fmap_ops_is_valid_access,
	.btf_struct_access = fuse_dax_fmap_ops_btf_struct_access,
};

static int fuse_dax_fmap_ops_init_member(const struct btf_type *t,
					 const struct btf_member *member,
					 void *kdata, const void *udata)
{
	const struct fuse_dax_fmap_ops *uops = udata;
	struct fuse_dax_fmap_ops *kops = kdata;
	u32 moff;

	moff = __btf_member_bit_offset(t, member) / 8;

	switch (moff) {
	case offsetof(struct fuse_dax_fmap_ops, name):
		if (uops->name[0] == '\0')
			return -EINVAL;
		memcpy(kops->name, uops->name, FUSE_DAX_FMAP_OPS_NAME_LEN);
		kops->name[FUSE_DAX_FMAP_OPS_NAME_LEN - 1] = '\0';
		return 1;
	case offsetof(struct fuse_dax_fmap_ops, meta_size):
		if (uops->meta_size == 0 ||
		    uops->meta_size > FUSE_DAX_FMAP_META_MAX)
			return -EINVAL;
		kops->meta_size = uops->meta_size;
		return 1;
	}
	return 0;
}

/*
 * Global list of registered fuse_dax_fmap_ops.
 * Protected by fuse_dax_fmap_ops_lock.
 */
static LIST_HEAD(fuse_dax_fmap_ops_list);
static DEFINE_SPINLOCK(fuse_dax_fmap_ops_lock);

struct fuse_dax_fmap_ops_entry {
	struct list_head list;
	struct fuse_dax_fmap_ops *ops;
	struct bpf_link *link;
};

static int fuse_dax_fmap_ops_reg(void *kdata, struct bpf_link *link)
{
	struct fuse_dax_fmap_ops *ops = kdata;
	struct fuse_dax_fmap_ops_entry *entry;

	if (!ops->iomap_setup || !ops->iomap_begin)
		return -EINVAL;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->ops = ops;
	entry->link = link;

	spin_lock(&fuse_dax_fmap_ops_lock);
	list_add_tail(&entry->list, &fuse_dax_fmap_ops_list);
	spin_unlock(&fuse_dax_fmap_ops_lock);

	return 0;
}

static void fuse_dax_fmap_ops_unreg(void *kdata, struct bpf_link *link)
{
	struct fuse_dax_fmap_ops_entry *entry, *tmp;

	spin_lock(&fuse_dax_fmap_ops_lock);
	list_for_each_entry_safe(entry, tmp, &fuse_dax_fmap_ops_list, list) {
		if (entry->ops == kdata) {
			list_del(&entry->list);
			kfree(entry);
			break;
		}
	}
	spin_unlock(&fuse_dax_fmap_ops_lock);
}

/**
 * fuse_dax_fmap_ops_find - Look up registered ops by name
 * @name: ops name to find
 * @linkp: out-param for bpf_link reference (caller must bpf_link_put)
 *
 * Returns a pointer to the ops on success, or NULL if not found.
 * On success, *linkp holds an incremented bpf_link reference that
 * the caller owns (typically stored on fuse_conn).
 */
struct fuse_dax_fmap_ops *fuse_dax_fmap_ops_find(const char *name,
						 struct bpf_link **linkp)
{
	struct fuse_dax_fmap_ops_entry *entry;
	struct fuse_dax_fmap_ops *ops = NULL;

	spin_lock(&fuse_dax_fmap_ops_lock);
	list_for_each_entry(entry, &fuse_dax_fmap_ops_list, list) {
		if (!strncmp(entry->ops->name, name,
			     FUSE_DAX_FMAP_OPS_NAME_LEN)) {
			bpf_link_inc(entry->link);
			ops = entry->ops;
			*linkp = entry->link;
			break;
		}
	}
	spin_unlock(&fuse_dax_fmap_ops_lock);

	return ops;
}
EXPORT_SYMBOL_GPL(fuse_dax_fmap_ops_find);

/* CFI stubs */
static int __fuse_dax_fmap_iomap_setup(struct fuse_dax_fmap_parse_ctx *ctx)
{
	return -EOPNOTSUPP;
}

static int __fuse_dax_fmap_iomap_begin(struct fuse_dax_fmap_resolve_ctx *ctx,
				       struct fuse_iomap_io *io)
{
	return -EOPNOTSUPP;
}

static struct fuse_dax_fmap_ops __bpf_fuse_dax_fmap_ops = {
	.iomap_setup = __fuse_dax_fmap_iomap_setup,
	.iomap_begin = __fuse_dax_fmap_iomap_begin,
};

static struct bpf_struct_ops bpf_fuse_dax_fmap_ops = {
	.verifier_ops = &fuse_dax_fmap_verifier_ops,
	.init = fuse_dax_fmap_ops_init,
	.check_member = fuse_dax_fmap_ops_check_member,
	.init_member = fuse_dax_fmap_ops_init_member,
	.reg = fuse_dax_fmap_ops_reg,
	.unreg = fuse_dax_fmap_ops_unreg,
	.name = "fuse_dax_fmap_ops",
	.cfi_stubs = &__bpf_fuse_dax_fmap_ops,
	.owner = THIS_MODULE,
};

/* kfuncs for BPF program access to context buffers */

__bpf_kfunc_start_defs();

/**
 * bpf_fuse_dax_setup_get_meta - Get write pointer to metadata buffer
 * @ctx: parse context (from iomap_setup)
 * @offset: byte offset into meta_buf
 * @rdwr_buf_size: const size of the region to access
 *
 * Returns NULL if offset+size exceeds meta_buf_size.
 */
__bpf_kfunc __u8 *
bpf_fuse_dax_setup_get_meta(struct fuse_dax_fmap_parse_ctx *ctx,
			    __u32 offset, const __u32 rdwr_buf_size)
{
	struct fuse_dax_fmap_parse_ctx_kern *kctx;

	kctx = container_of(ctx, struct fuse_dax_fmap_parse_ctx_kern, ctx);

	if ((u64)offset + rdwr_buf_size > ctx->meta_buf_size)
		return NULL;

	return (__u8 *)kctx->meta_buf + offset;
}

/**
 * bpf_fuse_dax_setup_add_device - Resolve a DAX device by path
 * @ctx: parse context (from iomap_setup)
 * @dev_index: device slot index
 * @path__buf: device path buffer (e.g. "/dev/dax0.0")
 * @path_len__sz: length of path buffer
 *
 * Calls kern_path + dax_dev_get + fs_dax_get for the given device path.
 * Must be called from sleepable iomap_setup context.
 */
__bpf_kfunc int
bpf_fuse_dax_setup_add_device(struct fuse_dax_fmap_parse_ctx *ctx,
			      __u32 dev_index,
			      const char *path__buf, __u32 path_len__sz)
{
	struct fuse_dax_fmap_parse_ctx_kern *kctx;
	char pathbuf[256];

	kctx = container_of(ctx, struct fuse_dax_fmap_parse_ctx_kern, ctx);

	if (!kctx->fc)
		return -EINVAL;

	if (path_len__sz == 0 || path_len__sz >= sizeof(pathbuf))
		return -EINVAL;

	memcpy(pathbuf, path__buf, path_len__sz);
	pathbuf[path_len__sz] = '\0';

	return fuse_dax_fmap_resolve_one_device(kctx->fc, dev_index, pathbuf);
}

/**
 * bpf_fuse_dax_resolve_get_meta - Get read pointer to metadata buffer
 * @ctx: resolve context
 * @offset: byte offset into meta_buf
 * @rdonly_buf_size: const size of the region to access
 *
 * Returns NULL if offset+size exceeds meta_buf_size.
 */
__bpf_kfunc const __u8 *
bpf_fuse_dax_resolve_get_meta(struct fuse_dax_fmap_resolve_ctx *ctx,
			      __u32 offset, const __u32 rdonly_buf_size)
{
	struct fuse_dax_fmap_resolve_ctx_kern *kctx;

	kctx = container_of(ctx, struct fuse_dax_fmap_resolve_ctx_kern, ctx);

	if ((u64)offset + rdonly_buf_size > ctx->meta_buf_size)
		return NULL;

	return (const __u8 *)kctx->meta_buf + offset;
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(fuse_dax_fmap_kfunc_ids)
BTF_ID_FLAGS(func, bpf_fuse_dax_setup_get_meta, KF_RET_NULL)
BTF_ID_FLAGS(func, bpf_fuse_dax_setup_add_device, KF_SLEEPABLE)
BTF_ID_FLAGS(func, bpf_fuse_dax_resolve_get_meta, KF_RET_NULL)
BTF_KFUNCS_END(fuse_dax_fmap_kfunc_ids)

static const struct btf_kfunc_id_set fuse_dax_fmap_kfunc_set = {
	.owner = THIS_MODULE,
	.set = &fuse_dax_fmap_kfunc_ids,
};

static int __init fuse_dax_fmap_struct_ops_init(void)
{
	int ret;

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					&fuse_dax_fmap_kfunc_set);
	if (ret)
		return ret;

	return register_bpf_struct_ops(&bpf_fuse_dax_fmap_ops,
				       fuse_dax_fmap_ops);
}
late_initcall(fuse_dax_fmap_struct_ops_init);
