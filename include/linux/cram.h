/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRAM_H
#define _LINUX_CRAM_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/range.h>

struct module;

/**
 * struct cram_ops - driver callbacks CRAM may invoke (core -> driver)
 * @owner: module providing the callbacks, THIS_MODULE.  Pins the module for the
 *         node's lifetime so CRAM never calls into unloaded code.  Normal rmmod
 *         is blocked until cram_unregister().  NULL for built-in drivers.
 * @trim:  optional.  Ask the device to drop its compression backing for the freed
 *         pages in @pfns (@nr_pages entries).  @result is a parallel per-page array
 *         (0 = ok, -1 = failed).  Return 0 = all ok, -EAGAIN = CRAM retries the
 *         failed pages, -EBUSY = CRAM zeroes the failed pages itself.  If @trim is
 *         NULL, CRAM zeroes freed pages instead.  @driver_data is from register.
 */
struct cram_ops {
	struct module *owner;
	int (*trim)(void *driver_data, const unsigned long *pfns,
		    int *result, unsigned int nr_pages);
};

#if IS_ENABLED(CONFIG_CRAM)

/*
 * cram_register(): @driver_data is the driver's per-node object, the cookie
 * passed back to ops callbacks and opaque to CRAM.  All keyed operations
 * (unregister, ratio, gate, hot-page) identify the node by @nid.  @zratio is
 * the hardware compression ratio, per-mille N:1 (3:1 => 3000, 1:1 => 1000).
 */
int cram_register(int nid, const struct range *ranges, unsigned int n,
		  u32 zratio, int adistance, struct cram_ops ops,
		  void *driver_data);
int cram_unregister(int nid, const struct range *ranges, unsigned int n);
int cram_set_compression_ratio(int nid, u32 ratio, bool block_alloc);
int cram_allow_allocation(int nid, bool allow);
int cram_report_hot_pages(int nid, const unsigned long *pfns, unsigned int n);

#else /* !CONFIG_CRAM */

static inline int cram_register(int nid, const struct range *ranges,
				unsigned int n, u32 zratio, int adistance,
				struct cram_ops ops, void *driver_data)
{
	return -ENODEV;
}

static inline int cram_unregister(int nid, const struct range *ranges,
				  unsigned int n)
{
	return -ENODEV;
}

static inline int cram_set_compression_ratio(int nid, u32 ratio,
					     bool block_alloc)
{
	return -ENODEV;
}

static inline int cram_allow_allocation(int nid, bool allow)
{
	return -ENODEV;
}

static inline int cram_report_hot_pages(int nid, const unsigned long *pfns,
					unsigned int n)
{
	return -ENODEV;
}

#endif /* CONFIG_CRAM */

#endif /* _LINUX_CRAM_H */
