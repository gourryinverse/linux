/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fuse_dax_fmap - FUSE DAX file mapping infrastructure
 *
 * Copyright 2023-2026 Micron Technology, Inc.
 */
#ifndef FUSE_DAX_FMAP_H
#define FUSE_DAX_FMAP_H

/*
 * fuse_daxdev - tracking struct for a daxdev within a FUSE DAX filesystem
 *
 * This is the in-memory daxdev metadata that is populated by resolving
 * device names provided by BPF parse() callbacks.
 */
struct fuse_daxdev {
	bool valid;
	bool error; /* Dax has reported a memory error (probably poison) */
	bool dax_err; /* fs_dax_get() failed */
	dev_t devno;
	struct dax_device *devp;
	char *name;
};

#define MAX_DAXDEVS 24

/*
 * fuse_dax_devlist - list of fuse_daxdev's
 */
struct fuse_dax_devlist {
	int nslots;
	int ndevs;
	struct fuse_daxdev *devlist;
};

struct fuse_conn;
int fuse_dax_fmap_resolve_one_device(struct fuse_conn *fc, u32 idx,
				     const char *name);

#endif /* FUSE_DAX_FMAP_H */