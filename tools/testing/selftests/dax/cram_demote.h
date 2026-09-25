/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cram_demote.h - request reclaim of an anonymous range.
 *
 * Drive placement through MADV_PAGEOUT, the userspace reclaim interface.
 */
#ifndef __CRAM_DEMOTE_H__
#define __CRAM_DEMOTE_H__

#include <stddef.h>
#include <sys/mman.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

/*
 * Returns the madvise() result.  Tests verify residency separately because
 * reclaim may legitimately retain or swap a folio instead of placing it on
 * CRAM.
 */
static inline int cram_pageout(void *addr, size_t len)
{
	return madvise(addr, len, MADV_PAGEOUT);
}

#endif /* __CRAM_DEMOTE_H__ */
