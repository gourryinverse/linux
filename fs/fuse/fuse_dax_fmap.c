// SPDX-License-Identifier: GPL-2.0
/*
 * fuse_dax_fmap - FUSE DAX file mapping infrastructure
 *
 * Copyright 2023-2026 Micron Technology, Inc.
 *
 * This file provides generic FUSE DAX fmap support, allowing FUSE
 * filesystems to map files directly onto devdax memory. Extent
 * resolution is handled by BPF struct_ops programs.
 */

#include <linux/bpf.h>
#include <linux/cleanup.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/fuse_dax_fmap_ops.h>
#include <linux/iomap.h>
#include <linux/limits.h>
#include <linux/pagemap.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/string.h>

#include "fuse_dax_fmap.h"
#include "fuse_i.h"

static void fuse_dax_set_daxdev_err(
	struct fuse_conn *fc, struct dax_device *dax_devp);

static int
fuse_dax_notify_failure(struct dax_device *dax_devp, u64 offset,
			u64 len, int mf_flags)
{
	struct fuse_conn *fc = dax_holder(dax_devp);

	fuse_dax_set_daxdev_err(fc, dax_devp);

	return 0;
}

static const struct dax_holder_operations fuse_dax_fmap_holder_ops = {
	.notify_failure		= fuse_dax_notify_failure,
};

static const struct address_space_operations fuse_dax_fmap_aops = {
	.dirty_folio	= noop_dirty_folio,
};

/*****************************************************************************/

void
fuse_dax_fmap_teardown(struct fuse_conn *fc)
{
	struct fuse_dax_devlist *devlist __free(kfree) = fc->dax_devlist;
	int i;

	fc->dax_devlist = NULL;

	if (fc->dax_fmap_link) {
		bpf_link_put(fc->dax_fmap_link);
		fc->dax_fmap_link = NULL;
		fc->dax_fmap_ops = NULL;
	}

	if (!devlist)
		return;

	if (!devlist->devlist)
		return;

	for (i = 0; i < devlist->nslots; i++) {
		struct fuse_daxdev *dd = &devlist->devlist[i];

		if (!dd->valid)
			continue;

		if (dd->devp) {
			if (!dd->dax_err)
				fs_put_dax(dd->devp, fc);
			put_dax(dd->devp);
		}

		kfree(dd->name);
	}
	kfree(devlist->devlist);
}

static int
fuse_dax_verify_daxdev(const char *pathname, dev_t *devno)
{
	struct inode *inode;
	struct path path;
	int err;

	if (!pathname || !*pathname)
		return -EINVAL;

	err = kern_path(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = d_backing_inode(path.dentry);
	if (!S_ISCHR(inode->i_mode)) {
		err = -EINVAL;
		goto out_path_put;
	}

	if (!may_open_dev(&path)) {
		err = -EACCES;
		goto out_path_put;
	}

	*devno = inode->i_rdev;

out_path_put:
	path_put(&path);
	return err;
}

static int
fuse_dax_fmap_alloc_devlist(struct fuse_conn *fc)
{
	struct fuse_dax_devlist *devlist;

	if (fc->dax_devlist)
		return 0;

	devlist = kcalloc(1, sizeof(*devlist), GFP_KERNEL);
	if (!devlist)
		return -ENOMEM;

	devlist->nslots = MAX_DAXDEVS;
	devlist->devlist = kcalloc(MAX_DAXDEVS, sizeof(struct fuse_daxdev),
				   GFP_KERNEL);
	if (!devlist->devlist) {
		kfree(devlist);
		return -ENOMEM;
	}

	if (cmpxchg(&fc->dax_devlist, NULL, devlist) != NULL) {
		kfree(devlist->devlist);
		kfree(devlist);
	}

	return 0;
}

int
fuse_dax_fmap_resolve_one_device(struct fuse_conn *fc, u32 idx,
				 const char *name)
{
	struct fuse_daxdev *daxdev;
	int rc;

	rc = fuse_dax_fmap_alloc_devlist(fc);
	if (rc)
		return rc;

	if (idx >= fc->dax_devlist->nslots) {
		pr_err("%s: dev_index %u >= nslots %d\n",
		       __func__, idx, fc->dax_devlist->nslots);
		return -EINVAL;
	}

	scoped_guard(rwsem_write, &fc->dax_devlist_sem) {
		daxdev = &fc->dax_devlist->devlist[idx];

		if (daxdev->valid)
			return 0;

		rc = fuse_dax_verify_daxdev(name, &daxdev->devno);
		if (rc)
			return rc;

		daxdev->name = kstrdup(name, GFP_KERNEL);
		if (!daxdev->name)
			return -ENOMEM;

		daxdev->devp = dax_dev_get(daxdev->devno);
		if (!daxdev->devp) {
			pr_warn("%s: device %s not found or not dax\n",
				__func__, name);
			kfree(daxdev->name);
			daxdev->name = NULL;
			return -ENODEV;
		}

		rc = fs_dax_get(daxdev->devp, fc, &fuse_dax_fmap_holder_ops);
		if (rc) {
			daxdev->dax_err = 1;
			pr_err("%s: fs_dax_get failed for %s\n",
			       __func__, name);
		}

		wmb();
		daxdev->valid = 1;
	}

	return 0;
}

static void
fuse_dax_set_daxdev_err(
	struct fuse_conn *fc,
	struct dax_device *dax_devp)
{
	int i;

	scoped_guard(rwsem_write, &fc->dax_devlist_sem) {
		for (i = 0; i < fc->dax_devlist->nslots; i++) {
			if (fc->dax_devlist->devlist[i].valid) {
				struct fuse_daxdev *dd;

				dd = &fc->dax_devlist->devlist[i];
				if (dd->devp != dax_devp)
					continue;

				dd->error = true;

				pr_err("%s: memory error on daxdev %s (%d)\n",
				       __func__, dd->name, i);
				return;
			}
		}
	}
	pr_err("%s: memory err on unrecognized daxdev\n", __func__);
}

/***************************************************************************/

void __fuse_dax_fmap_meta_free(struct fuse_inode *fi)
{
	if (!fi->dax_fmap.meta)
		return;

	kfree(fi->dax_fmap.meta);
	fi->dax_fmap.meta = NULL;
}

/*********************************************************************
 * iomap_operations
 */

static int fuse_dax_file_bad(struct inode *inode);

static int fuse_dax_err(struct fuse_daxdev *dd)
{
	if (!dd->valid) {
		pr_err("%s: daxdev=%s invalid\n",
		       __func__, dd->name);
		return -EIO;
	}
	if (dd->dax_err) {
		pr_err("%s: daxdev=%s dax_err\n",
		       __func__, dd->name);
		return -EIO;
	}
	if (dd->error) {
		pr_err("%s: daxdev=%s memory error\n",
		       __func__, dd->name);
		return -EHWPOISON;
	}
	return 0;
}

static int
fuse_dax_fmap_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		  unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_dax_fmap_resolve_ctx_kern kern;
	struct fuse_iomap_io io = {};
	struct fuse_dax_fmap_ops *ops = fc->dax_fmap_ops;
	struct fuse_daxdev *dd;
	int rc;

	if (!ops)
		return -EOPNOTSUPP;

	if (!fc->dax_devlist)
		return -EIO;

	kern.meta_buf = fi->dax_fmap.meta;
	kern.ctx = (struct fuse_dax_fmap_resolve_ctx){
		.meta_buf_size = fi->dax_fmap.meta_size,
		.file_offset   = offset,
		.length        = length,
	};

	rc = ops->iomap_begin(&kern.ctx, &io);
	if (rc)
		return rc;

	if (io.dev_index >= fc->dax_devlist->nslots) {
		pr_err("%s: dev_index %u >= nslots %d\n",
		       __func__, io.dev_index,
		       fc->dax_devlist->nslots);
		return -EIO;
	}

	dd = &fc->dax_devlist->devlist[io.dev_index];

	rc = fuse_dax_err(dd);
	if (rc)
		return rc;

	iomap->offset  = offset;
	iomap->addr    = io.addr;
	iomap->length  = io.length;
	iomap->dax_dev = dd->devp;
	iomap->type    = IOMAP_MAPPED;
	iomap->flags   = flags;

	return 0;
}

const struct iomap_ops fuse_dax_fmap_iomap_ops = {
	.iomap_begin		= fuse_dax_fmap_iomap_begin,
};

/*********************************************************************
 * vm_operations
 */
static vm_fault_t
__fuse_dax_fmap_filemap_fault(struct vm_fault *vmf, unsigned int pe_size,
		      bool write_fault)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	vm_fault_t ret;
	unsigned long pfn;

	if (!IS_DAX(file_inode(vmf->vma->vm_file))) {
		pr_err("%s: file not marked IS_DAX!!\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	if (write_fault) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(vmf->vma->vm_file);
	}

	ret = dax_iomap_fault(vmf, pe_size, &pfn, NULL, &fuse_dax_fmap_iomap_ops);
	if (ret & VM_FAULT_NEEDDSYNC)
		ret = dax_finish_sync_fault(vmf, pe_size, pfn);

	if (write_fault)
		sb_end_pagefault(inode->i_sb);

	return ret;
}

static inline bool
fuse_dax_is_write_fault(struct vm_fault *vmf)
{
	return (vmf->flags & FAULT_FLAG_WRITE) &&
	       (vmf->vma->vm_flags & VM_SHARED);
}

static vm_fault_t
fuse_dax_filemap_fault(struct vm_fault *vmf)
{
	return __fuse_dax_fmap_filemap_fault(vmf, 0, fuse_dax_is_write_fault(vmf));
}

static vm_fault_t
fuse_dax_filemap_huge_fault(struct vm_fault *vmf, unsigned int pe_size)
{
	return __fuse_dax_fmap_filemap_fault(vmf, pe_size,
					  fuse_dax_is_write_fault(vmf));
}

static vm_fault_t
fuse_dax_filemap_mkwrite(struct vm_fault *vmf)
{
	return __fuse_dax_fmap_filemap_fault(vmf, 0, true);
}

const struct vm_operations_struct fuse_dax_fmap_vm_ops = {
	.fault		= fuse_dax_filemap_fault,
	.huge_fault	= fuse_dax_filemap_huge_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= fuse_dax_filemap_mkwrite,
	.pfn_mkwrite	= fuse_dax_filemap_mkwrite,
};

/*********************************************************************
 * file_operations
 */

static int
fuse_dax_file_bad(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	if (!fi->dax_fmap.meta) {
		pr_err("%s: un-initialized dax fmap file\n", __func__);
		return -EIO;
	}
	if (!IS_DAX(inode)) {
		pr_debug("%s: inode %llx IS_DAX is false\n",
			 __func__, (u64)inode);
		return -ENXIO;
	}
	return 0;
}

static ssize_t
fuse_dax_fmap_rw_prep(struct kiocb *iocb, struct iov_iter *ubuf)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	size_t i_size = i_size_read(inode);
	size_t count = iov_iter_count(ubuf);
	size_t max_count;
	ssize_t rc;

	rc = fuse_dax_file_bad(inode);
	if (rc)
		return (ssize_t)rc;

	if (iocb->ki_pos >= i_size)
		max_count = 0;
	else
		max_count = i_size - iocb->ki_pos;

	if (count > max_count)
		iov_iter_truncate(ubuf, max_count);

	if (!iov_iter_count(ubuf))
		return 0;

	return rc;
}

ssize_t
fuse_dax_fmap_read_iter(struct kiocb *iocb, struct iov_iter	*to)
{
	ssize_t rc;

	rc = fuse_dax_fmap_rw_prep(iocb, to);
	if (rc)
		return rc;

	if (!iov_iter_count(to))
		return 0;

	rc = dax_iomap_rw(iocb, to, &fuse_dax_fmap_iomap_ops);

	file_accessed(iocb->ki_filp);
	return rc;
}

ssize_t
fuse_dax_fmap_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t rc;

	rc = fuse_dax_fmap_rw_prep(iocb, from);
	if (rc)
		return rc;

	if (!iov_iter_count(from))
		return 0;

	return dax_iomap_rw(iocb, from, &fuse_dax_fmap_iomap_ops);
}

int
fuse_dax_fmap_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);
	ssize_t rc;

	rc = fuse_dax_file_bad(inode);
	if (rc)
		return rc;

	file_accessed(file);
	vma->vm_ops = &fuse_dax_fmap_vm_ops;
	vm_flags_set(vma, VM_HUGEPAGE);
	return 0;
}

int fuse_dax_fmap_open(struct fuse_mount *fm, struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_conn *fc = fm->fc;
	struct fuse_dax_fmap_ops *ops = fc->dax_fmap_ops;
	struct fuse_dax_fmap_parse_ctx_kern kern;
	void *meta_buf = NULL;
	u32 meta_size;
	int rc;

	if (fi->dax_fmap.meta)
		return 0;

	if (!ops)
		return -EOPNOTSUPP;

	meta_size = ops->meta_size;

	meta_buf = kzalloc(meta_size, GFP_KERNEL);
	if (!meta_buf)
		return -ENOMEM;

	kern = (struct fuse_dax_fmap_parse_ctx_kern){
		.ctx = {
			.nodeid        = get_node_id(inode),
			.meta_buf_size = meta_size,
		},
		.meta_buf = meta_buf,
		.fc       = fc,
	};

	rc = ops->iomap_setup(&kern.ctx);
	if (rc) {
		pr_err("%s: BPF iomap_setup failed: %d\n", __func__, rc);
		goto err_free_meta;
	}

	inode_lock(inode);

	if (fi->dax_fmap.meta) {
		inode_unlock(inode);
		kfree(meta_buf);
		return 0;
	}

	fi->dax_fmap.meta      = meta_buf;
	fi->dax_fmap.meta_size = meta_size;
	inode->i_flags |= S_DAX;
	inode->i_data.a_ops = &fuse_dax_fmap_aops;

	inode_unlock(inode);
	return 0;

err_free_meta:
	kfree(meta_buf);
	return rc;
}
