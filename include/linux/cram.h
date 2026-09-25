/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRAM_H
#define _LINUX_CRAM_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/migrate_mode.h>
#include <linux/range.h>

struct module;
struct folio;

/**
 * struct cram_ops - driver callbacks CRAM may invoke (core -> driver)
 * @owner: module providing the callbacks, THIS_MODULE.  Pins the module for the
 *         node's lifetime so CRAM never calls into unloaded code.  Normal rmmod
 *         is blocked until cram_unregister().  NULL for built-in drivers.
 * @trim:  optional.  Ask the device to drop its compression backing for the
 *         contiguous range [@start_pfn, @start_pfn + @nr_pages).  On failure,
 *         or if @trim is NULL, CRAM zeroes the range instead.  @driver_data is
 *         from register.
 */
struct cram_ops {
	struct module *owner;
	int (*trim)(void *driver_data, unsigned long start_pfn,
		    unsigned long nr_pages);
};

#if IS_ENABLED(CONFIG_CRAM)

/*
 * cram_register(): @driver_data is the driver's per-node object, the cookie
 * passed back to ops callbacks and opaque to CRAM.  All keyed operations
 * (unregister, capacity and allocation gate) identify the node by @nid.
 * @features must include RECLAIM and WR_FENCE and must exclude COMMON;
 * COMPACTION is optional.
 */
int cram_register(int nid, const struct range *ranges, unsigned int n,
		  unsigned long features, struct cram_ops ops,
		  void *driver_data);
int cram_unregister(int nid, const struct range *ranges, unsigned int n);
int cram_set_capacity(int nid, unsigned long nr_pages);
int cram_set_no_alloc(int nid, bool no_alloc);
bool cram_can_demote(int src_nid);

bool cram_folio_eligible(struct folio *folio);
int cram_migrate_to(struct list_head *folios, enum migrate_mode mode,
		    enum migrate_reason reason, unsigned int *nr_succeeded);

#else /* !CONFIG_CRAM */

static inline int cram_register(int nid, const struct range *ranges,
				unsigned int n, unsigned long features,
				struct cram_ops ops, void *driver_data)
{
	return -ENODEV;
}

static inline int cram_unregister(int nid, const struct range *ranges,
				  unsigned int n)
{
	return -ENODEV;
}

static inline int cram_set_capacity(int nid, unsigned long nr_pages)
{
	return -ENODEV;
}

static inline int cram_set_no_alloc(int nid, bool no_alloc)
{
	return -ENODEV;
}

static inline bool cram_can_demote(int src_nid)
{
	return false;
}

static inline bool cram_folio_eligible(struct folio *folio)
{
	return false;
}

static inline int cram_migrate_to(struct list_head *folios,
				  enum migrate_mode mode,
				  enum migrate_reason reason,
				  unsigned int *nr_succeeded)
{
	return -ENODEV;
}

#endif /* CONFIG_CRAM */

#endif /* _LINUX_CRAM_H */
