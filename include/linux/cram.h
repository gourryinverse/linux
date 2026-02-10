/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRAM_H
#define _LINUX_CRAM_H

#include <linux/mm_types.h>

struct folio;
struct list_head;
struct vm_fault;

#define CRAM_PRESSURE_MAX	1000

/**
 * cram_flush_cb_t - Driver callback invoked when a folio on a private node
 *                   is freed (refcount reaches zero).
 * @folio: the folio being freed
 * @private: opaque driver data passed at registration
 *
 * Return:
 *   0: Flush resolved -- page should return to buddy allocator (e.g., flush
 *      record bit was set, meaning this free is from our own flush resolution)
 *   1: Page deferred -- driver took a reference, page will be flushed later.
 *      Do NOT return to buddy allocator.
 *   2: Buffer full -- caller should zero the page and return to buddy.
 */
typedef int (*cram_flush_cb_t)(struct folio *folio, void *private);

#ifdef CONFIG_CRAM

int cram_register_private_node(int nid, void *owner,
			       cram_flush_cb_t flush_cb, void *flush_data);
int cram_unregister_private_node(int nid);
int cram_unpurge(int nid);
void cram_set_pressure(int nid, unsigned int pressure);
void cram_clear_pressure(int nid);

#else /* !CONFIG_CRAM */

static inline int cram_register_private_node(int nid, void *owner,
					     cram_flush_cb_t flush_cb,
					     void *flush_data)
{
	return -ENODEV;
}

static inline int cram_unregister_private_node(int nid)
{
	return -ENODEV;
}

static inline int cram_unpurge(int nid)
{
	return -ENODEV;
}

static inline void cram_set_pressure(int nid, unsigned int pressure)
{
}

static inline void cram_clear_pressure(int nid)
{
}

#endif /* CONFIG_CRAM */

#endif /* _LINUX_CRAM_H */
