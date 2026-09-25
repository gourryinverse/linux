/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRAM_H
#define _LINUX_CRAM_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/migrate_mode.h>
#include <linux/range.h>

struct folio;

#if IS_ENABLED(CONFIG_CRAM)

/*
 * @features must include RECLAIM and WR_FENCE and must exclude COMMON;
 * COMPACTION is optional.
 */
int cram_register(int nid, const struct range *ranges, unsigned int n,
		  unsigned long features);
int cram_unregister(int nid, const struct range *ranges, unsigned int n);
bool cram_can_demote(int src_nid);

bool cram_folio_eligible(struct folio *folio);
int cram_migrate_to(struct list_head *folios, enum migrate_mode mode,
		    enum migrate_reason reason, unsigned int *nr_succeeded);

#else /* !CONFIG_CRAM */

static inline int cram_register(int nid, const struct range *ranges,
				unsigned int n, unsigned long features)
{
	return -ENODEV;
}

static inline int cram_unregister(int nid, const struct range *ranges,
				  unsigned int n)
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
