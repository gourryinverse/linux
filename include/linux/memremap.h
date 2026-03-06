/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMREMAP_H_
#define _LINUX_MEMREMAP_H_

#include <linux/mmzone.h>
#include <linux/range.h>
#include <linux/ioport.h>
#include <linux/percpu-refcount.h>

struct resource;
struct device;

/**
 * struct vmem_altmap - pre-allocated storage for vmemmap_populate
 * @base_pfn: base of the entire dev_pagemap mapping
 * @reserve: pages mapped, but reserved for driver use (relative to @base)
 * @free: free pages set aside in the mapping for memmap storage
 * @align: pages reserved to meet allocation alignments
 * @alloc: track pages consumed, private to vmemmap_populate()
 */
struct vmem_altmap {
	unsigned long base_pfn;
	const unsigned long end_pfn;
	const unsigned long reserve;
	unsigned long free;
	unsigned long align;
	unsigned long alloc;
};

/*
 * Specialize ZONE_DEVICE memory into multiple types each has a different
 * usage.
 *
 * MEMORY_DEVICE_PRIVATE:
 * Device memory that is not directly addressable by the CPU: CPU can neither
 * read nor write private memory. In this case, we do still have struct pages
 * backing the device memory. Doing so simplifies the implementation, but it is
 * important to remember that there are certain points at which the struct page
 * must be treated as an opaque object, rather than a "normal" struct page.
 *
 * A more complete discussion of unaddressable memory may be found in
 * include/linux/hmm.h and Documentation/mm/hmm.rst.
 *
 * MEMORY_DEVICE_COHERENT:
 * Device memory that is cache coherent from device and CPU point of view. This
 * is used on platforms that have an advanced system bus (like CAPI or CXL). A
 * driver can hotplug the device memory using ZONE_DEVICE and with that memory
 * type. Any page of a process can be migrated to such memory. However no one
 * should be allowed to pin such memory so that it can always be evicted.
 *
 * MEMORY_DEVICE_FS_DAX:
 * Host memory that has similar access semantics as System RAM i.e. DMA
 * coherent and supports page pinning. In support of coordinating page
 * pinning vs other operations MEMORY_DEVICE_FS_DAX arranges for a
 * wakeup event whenever a page is unpinned and becomes idle. This
 * wakeup is used to coordinate physical address space management (ex:
 * fs truncate/hole punch) vs pinned pages (ex: device dma).
 *
 * MEMORY_DEVICE_GENERIC:
 * Host memory that has similar access semantics as System RAM i.e. DMA
 * coherent and supports page pinning. This is for example used by DAX devices
 * that expose memory using a character device.
 *
 * MEMORY_DEVICE_PCI_P2PDMA:
 * Device memory residing in a PCI BAR intended for use with Peer-to-Peer
 * transactions.
 *
 * MEMORY_DEVICE_MANAGED:
 * CPU-accessible device memory managed by a driver service (e.g. compressed
 * memory, accelerator memory).  Pages are hotplugged into the buddy allocator
 * via __add_memory_driver_managed() and live in normal zones (ZONE_NORMAL,
 * ZONE_MOVABLE).  folio_is_device_managed() identifies these pages via the
 * N_MEMORY_PRIVATE node state.  Unlike MEMORY_DEVICE_PRIVATE, the CPU can
 * read and write these pages directly.  The driver controls lifecycle through
 * dev_pagemap_ops callbacks registered on a per-node struct node_device
 * container (pgdat->node_dev).  __GFP_PRIVATE gates buddy allocations from
 * these nodes.
 */
enum memory_type {
	/* 0 is reserved to catch uninitialized type fields */
	MEMORY_DEVICE_PRIVATE = 1,
	MEMORY_DEVICE_COHERENT,
	MEMORY_DEVICE_FS_DAX,
	MEMORY_DEVICE_GENERIC,
	MEMORY_DEVICE_PCI_P2PDMA,
	MEMORY_DEVICE_MANAGED,
};

struct dev_pagemap_ops {
	/*
	 * Called once the folio refcount reaches 0.  The reference count will be
	 * reset to one by the core code after the method is called to prepare
	 * for handing out the folio again.
	 */
	void (*folio_free)(struct folio *folio);

	/*
	 * Called when a managed (node device) folio's refcount reaches 0.
	 * Returns true if the driver defers freeing (holds its own ref),
	 * false if the folio should continue through the normal buddy
	 * free path.  Not used by zone_device folios.
	 */
	bool (*free_node_folio)(struct folio *folio);

	/*
	 * Used for private (un-addressable) device memory only.  Must migrate
	 * the page back to a CPU accessible page.
	 */
	vm_fault_t (*migrate_to_ram)(struct vm_fault *vmf);

	/*
	 * Handle the memory failure happens on a range of pfns.  Notify the
	 * processes who are using these pfns, and try to recover the data on
	 * them if necessary.  The mf_flags is finally passed to the recover
	 * function through the whole notify routine.
	 *
	 * When this is not implemented, or it returns -EOPNOTSUPP, the caller
	 * will fall back to a common handler called mf_generic_kill_procs().
	 */
	int (*memory_failure)(struct dev_pagemap *pgmap, unsigned long pfn,
			      unsigned long nr_pages, int mf_flags);

	/*
	 * Called when a large folio is split into smaller folios.
	 * Used for device_private and device_managed memory types.
	 */
	void (*folio_split)(struct folio *head, struct folio *tail);

	/*
	 * Post-migration notification that a managed folio changed physical
	 * location.  Called from migrate_folio_move() after data has been
	 * copied but before migration entries are replaced with real PTEs.
	 * Both @src and @dst are locked.  Faults block in
	 * migration_entry_wait() so the service can safely update PFN-based
	 * metadata before any access through the page tables.
	 */
	void (*folio_migrate)(struct folio *src, struct folio *dst);

	/*
	 * Handle a write fault on a write-protected managed folio.
	 * Called from handle_pte_fault() (PTE level) or do_huge_pmd_wp_page()
	 * (PMD level) with PTL held.  The callback MUST release PTL on ALL
	 * paths.  Returns vm_fault_t result (0, VM_FAULT_RETRY, etc.).
	 */
	vm_fault_t (*handle_fault)(struct folio *folio, struct vm_fault *vmf,
				   unsigned int level);
};

#define PGMAP_ALTMAP_VALID	(1 << 0)
#define PGMAP_OPS_MIGRATION	(1 << 1)
#define PGMAP_OPS_MEMPOLICY	(1 << 2)
#define PGMAP_OPS_DEMOTION	(1 << 3)
#define PGMAP_OPS_PROTECT_WRITE	(1 << 4)

/**
 * struct dev_pagemap - metadata for ZONE_DEVICE mappings
 * @altmap: pre-allocated/reserved memory for vmemmap allocations
 * @ref: reference count that pins the devm_memremap_pages() mapping
 * @done: completion for @ref
 * @type: memory type: see MEMORY_* above in memremap.h
 * @flags: PGMAP_* flags to specify defailed behavior
 * @vmemmap_shift: structural definition of how the vmemmap page metadata
 *      is populated, specifically the metadata page order.
 *	A zero value (default) uses base pages as the vmemmap metadata
 *	representation. A bigger value will set up compound struct pages
 *	of the requested order value.
 * @ops: method table
 * @owner: an opaque pointer identifying the entity that manages this
 *	instance.  Used by various helpers to make sure that no
 *	foreign ZONE_DEVICE memory is accessed.
 * @nr_range: number of ranges to be mapped
 * @range: range to be mapped when nr_range == 1
 * @ranges: array of ranges to be mapped when nr_range > 1
 */
struct dev_pagemap {
	struct vmem_altmap altmap;
	struct percpu_ref ref;
	struct completion done;
	enum memory_type type;
	unsigned int flags;
	unsigned long vmemmap_shift;
	const struct dev_pagemap_ops *ops;
	void *owner;
	int nr_range;
	union {
		struct range range;
		DECLARE_FLEX_ARRAY(struct range, ranges);
	};
};

static inline bool pgmap_has_memory_failure(struct dev_pagemap *pgmap)
{
	return pgmap->ops && pgmap->ops->memory_failure;
}

static inline struct vmem_altmap *pgmap_altmap(struct dev_pagemap *pgmap)
{
	if (pgmap->flags & PGMAP_ALTMAP_VALID)
		return &pgmap->altmap;
	return NULL;
}

static inline unsigned long pgmap_vmemmap_nr(struct dev_pagemap *pgmap)
{
	return 1 << pgmap->vmemmap_shift;
}

static inline bool folio_is_device_private(const struct folio *folio)
{
	return IS_ENABLED(CONFIG_DEVICE_PRIVATE) &&
		folio_is_zone_device(folio) &&
		folio->pgmap->type == MEMORY_DEVICE_PRIVATE;
}

static inline bool is_device_private_page(const struct page *page)
{
	return IS_ENABLED(CONFIG_DEVICE_PRIVATE) &&
		folio_is_device_private(page_folio(page));
}

static inline bool folio_is_pci_p2pdma(const struct folio *folio)
{
	return IS_ENABLED(CONFIG_PCI_P2PDMA) &&
		folio_is_zone_device(folio) &&
		folio->pgmap->type == MEMORY_DEVICE_PCI_P2PDMA;
}

static inline void *folio_zone_device_data(const struct folio *folio)
{
	VM_WARN_ON_FOLIO(!folio_is_device_private(folio), folio);
	return folio->page.zone_device_data;
}

static inline void folio_set_zone_device_data(struct folio *folio, void *data)
{
	VM_WARN_ON_FOLIO(!folio_is_device_private(folio), folio);
	folio->page.zone_device_data = data;
}

static inline bool is_pci_p2pdma_page(const struct page *page)
{
	return IS_ENABLED(CONFIG_PCI_P2PDMA) &&
		folio_is_pci_p2pdma(page_folio(page));
}

static inline bool folio_is_device_coherent(const struct folio *folio)
{
	return folio_is_zone_device(folio) &&
		folio->pgmap->type == MEMORY_DEVICE_COHERENT;
}

static inline bool is_device_coherent_page(const struct page *page)
{
	return folio_is_device_coherent(page_folio(page));
}

static inline bool folio_is_fsdax(const struct folio *folio)
{
	return folio_is_zone_device(folio) &&
		folio->pgmap->type == MEMORY_DEVICE_FS_DAX;
}

static inline bool is_fsdax_page(const struct page *page)
{
	return folio_is_fsdax(page_folio(page));
}

static inline bool folio_is_device_managed(const struct folio *folio)
{
#if defined(CONFIG_ZONE_DEVICE) && defined(CONFIG_NUMA) && \
    !defined(NODE_NOT_IN_PAGE_FLAGS)
	return node_state((int)((folio->flags.f >> NODES_PGSHIFT) & NODES_MASK),
			  N_MEMORY_PRIVATE);
#else
	return false;
#endif
}

static inline bool is_device_managed_page(const struct page *page)
{
	return folio_is_device_managed(page_folio(page));
}

#ifdef CONFIG_ZONE_DEVICE
void zone_device_page_init(struct page *page, unsigned int order);
void *memremap_pages(struct dev_pagemap *pgmap, int nid);
void memunmap_pages(struct dev_pagemap *pgmap);
void *devm_memremap_pages(struct device *dev, struct dev_pagemap *pgmap);
void devm_memunmap_pages(struct device *dev, struct dev_pagemap *pgmap);
struct dev_pagemap *get_dev_pagemap(unsigned long pfn);
bool pgmap_pfn_valid(struct dev_pagemap *pgmap, unsigned long pfn);

unsigned long memremap_compat_align(void);

static inline void zone_device_folio_init(struct folio *folio, unsigned int order)
{
	zone_device_page_init(&folio->page, order);
	if (order)
		folio_set_large_rmappable(folio);
}

static inline void zone_device_private_split_cb(struct folio *original_folio,
						struct folio *new_folio)
{
	if (folio_is_device_private(original_folio)) {
		if (!original_folio->pgmap->ops->folio_split) {
			if (new_folio) {
				new_folio->pgmap = original_folio->pgmap;
				new_folio->page.mapping =
					original_folio->page.mapping;
			}
		} else {
			original_folio->pgmap->ops->folio_split(original_folio,
								 new_folio);
		}
	}
	/*
	 * Buddy-managed private node pages (folio_is_device_managed) do not
	 * have per-page pgmap pointers.  Split callbacks for these pages are
	 * dispatched via folio_managed_split_cb() using node_device_find_pgmap()
	 * to look up the pgmap by PFN.
	 */
}

#else
static inline void *devm_memremap_pages(struct device *dev,
		struct dev_pagemap *pgmap)
{
	/*
	 * Fail attempts to call devm_memremap_pages() without
	 * ZONE_DEVICE support enabled, this requires callers to fall
	 * back to plain devm_memremap() based on config
	 */
	WARN_ON_ONCE(1);
	return ERR_PTR(-ENXIO);
}

static inline void devm_memunmap_pages(struct device *dev,
		struct dev_pagemap *pgmap)
{
}

static inline struct dev_pagemap *get_dev_pagemap(unsigned long pfn)
{
	return NULL;
}

static inline bool pgmap_pfn_valid(struct dev_pagemap *pgmap, unsigned long pfn)
{
	return false;
}

/* when memremap_pages() is disabled all archs can remap a single page */
static inline unsigned long memremap_compat_align(void)
{
	return PAGE_SIZE;
}

static inline void zone_device_private_split_cb(struct folio *original_folio,
						struct folio *new_folio)
{
}
#endif /* CONFIG_ZONE_DEVICE */

static inline void put_dev_pagemap(struct dev_pagemap *pgmap)
{
	if (pgmap)
		percpu_ref_put(&pgmap->ref);
}

#endif /* _LINUX_MEMREMAP_H_ */
