/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRAM_H
#define _LINUX_CRAM_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/nodemask.h>
#include <linux/range.h>
#include <linux/jump_label.h>
#include <linux/migrate_mode.h>

struct module;
struct folio;
struct address_space;

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

#ifdef CONFIG_CRAM

/*
 * cram_register(): @driver_data is the driver's per-node object, the cookie
 * passed back to ops callbacks and opaque to CRAM.  All keyed operations
 * (unregister, ratio, gate, hot-page) identify the node by @nid.  @zratio is
 * the hardware compression ratio, per-mille N:1 (3:1 => 3000, 1:1 => 1000).
 */
int cram_register(int nid, const struct range *ranges, unsigned int n,
		  u32 zratio, struct cram_ops ops, void *driver_data);
int cram_unregister(int nid, const struct range *ranges, unsigned int n);
int cram_set_compression_ratio(int nid, u32 ratio, bool block_alloc);
int cram_allow_allocation(int nid, bool allow);
int cram_report_hot_pages(int nid, const unsigned long *pfns, unsigned int n);

/*
 * CRAM read-only tier core-mm hooks (bodies in mm/cram.c).
 *
 * CRAM folios live present read-only on an N_MEMORY_PRIVATE node.  Reads are
 * zero-copy.  A write must promote the folio back to DRAM rather than reuse it
 * in place: the device has no write path, and a writable page on it would
 * exceed real capacity.
 */
DECLARE_STATIC_KEY_FALSE(cram_enabled);
extern nodemask_t cram_node_mask;		/* set of registered CRAM nodes */
bool __folio_is_cram(struct folio *folio);

/**
 * node_is_cram() - is @nid a CRAM-managed private node?
 * @nid: the node to test
 *
 * Gated by a static key.  A kernel built with CONFIG_CRAM but no CRAM device
 * registered pays a patched NOP here, not a nodemask test.
 */
static inline bool node_is_cram(int nid)
{
	if (!static_branch_unlikely(&cram_enabled))
		return false;
	return node_isset(nid, cram_node_mask);
}

/**
 * folio_is_cram() - is this folio resident on a CRAM-managed private node?
 * @folio: the folio to test
 *
 * node_is_cram() on the folio's node.  The __folio_is_cram() call is
 * out-of-line only to keep folio_nid(), and thus <linux/mm.h>, out of this
 * header.  TODO: once the folio/memdesc descriptor split lands, an identity bit
 * in the descriptor could carry this directly and drop the nodemask test.
 */
static inline bool folio_is_cram(struct folio *folio)
{
	if (!static_branch_unlikely(&cram_enabled))
		return false;
	return __folio_is_cram(folio);
}
bool cram_can_demote(int src_nid);
bool cram_folio_eligible(struct folio *folio);
int cram_migrate_to(struct list_head *demote_folios, int src_nid,
		    enum migrate_mode mode, enum migrate_reason reason,
		    unsigned int *nr_succeeded);

/*
 * Resident file-cache tier (body in mm/cram.c).  A clean file folio demoted
 * onto a CRAM node stays in the page cache, mapped read-only in place.  A byte
 * writer promotes it back to DRAM first.  cram_promote_pagecache() migrates the
 * resident folio at (mapping, index) off the tier and is the action behind the
 * write-fence gates in filemap/memory/mprotect.  Returns 0 when promoted or
 * already gone, or -EAGAIN on transient failure (caller retries unless @nowait).
 */
int cram_promote_pagecache(struct address_space *mapping, pgoff_t index,
			   bool nowait);

#else /* !CONFIG_CRAM */

static inline int cram_register(int nid, const struct range *ranges,
				unsigned int n, u32 zratio,
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

static inline bool node_is_cram(int nid)
{
	return false;
}
static inline bool folio_is_cram(struct folio *folio)
{
	return false;
}
static inline bool cram_can_demote(int src_nid)
{
	return false;
}
static inline bool cram_folio_eligible(struct folio *folio)
{
	return false;
}
static inline int cram_migrate_to(struct list_head *demote_folios, int src_nid,
				  enum migrate_mode mode,
				  enum migrate_reason reason,
				  unsigned int *nr_succeeded)
{
	return -ENODEV;
}
static inline int cram_promote_pagecache(struct address_space *mapping,
					 pgoff_t index, bool nowait)
{
	return 0;
}

#endif /* CONFIG_CRAM */

#endif /* _LINUX_CRAM_H */
