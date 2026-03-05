// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Meta Platforms, Inc. All rights reserved. */
/*
 * CXL Mempolicy Driver
 *
 * Minimal driver for CXL memory devices that registers memory as a
 * private node with mempolicy support but no PTE controls.  The
 * memory behaves like normal DRAM but is isolated from default allocations,
 * it can only be reached via explicit mempolicy (set_mempolicy/mbind).
 *
 * Usage:
 *   1. Unbind device from cxl_pci:
 *        echo $PCI_DEV > /sys/bus/pci/drivers/cxl_pci/unbind
 *   2. Bind to cxl_mempolicy:
 *        echo $PCI_DEV > /sys/bus/pci/drivers/cxl_mempolicy/bind
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/xarray.h>
#include <linux/node_private.h>
#include <linux/migrate.h>
#include <cxl/mailbox.h>
#include "cxlmem.h"
#include "cxl.h"

struct cxl_mempolicy_ctx {
	struct cxl_region *cxlr;
	struct cxl_endpoint_decoder *cxled;
	int nid;
};

static DEFINE_XARRAY(ctx_xa);

static struct cxl_mempolicy_ctx *memdev_to_ctx(struct cxl_memdev *cxlmd)
{
	struct pci_dev *pdev = to_pci_dev(cxlmd->dev.parent);

	return xa_load(&ctx_xa, (unsigned long)pdev);
}

static int cxl_mempolicy_migrate_to(struct list_head *folios, int nid,
				    enum migrate_mode mode,
				    enum migrate_reason reason,
				    unsigned int *nr_succeeded)
{
	struct migration_target_control mtc = {
		.nid = nid,
		.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_THISNODE |
			    __GFP_PRIVATE,
		.reason = reason,
	};

	return migrate_pages(folios, alloc_migration_target, NULL,
			     (unsigned long)&mtc, mode, reason, nr_succeeded);
}

static void cxl_mempolicy_folio_migrate(struct folio *src, struct folio *dst)
{
}

static const struct node_private_ops cxl_mempolicy_ops = {
	.migrate_to	= cxl_mempolicy_migrate_to,
	.folio_migrate	= cxl_mempolicy_folio_migrate,
	.flags = NP_OPS_MIGRATION | NP_OPS_MEMPOLICY,
};

static struct cxl_region *create_ram_region(struct cxl_memdev *cxlmd)
{
	struct cxl_mempolicy_ctx *ctx = memdev_to_ctx(cxlmd);
	struct cxl_root_decoder *cxlrd;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_region *cxlr;
	resource_size_t ram_size, avail;

	ram_size = cxl_ram_size(cxlmd->cxlds);
	if (ram_size == 0) {
		dev_info(&cxlmd->dev, "no RAM capacity available\n");
		return ERR_PTR(-ENODEV);
	}

	ram_size = ALIGN_DOWN(ram_size, SZ_256M);
	if (ram_size == 0) {
		dev_info(&cxlmd->dev,
			 "RAM capacity too small (< 256M)\n");
		return ERR_PTR(-ENOSPC);
	}

	dev_info(&cxlmd->dev, "creating RAM region for %lld MB\n",
		 ram_size >> 20);

	cxlrd = cxl_get_hpa_freespace(cxlmd, ram_size, &avail);
	if (IS_ERR(cxlrd)) {
		dev_err(&cxlmd->dev, "no HPA freespace: %ld\n",
			PTR_ERR(cxlrd));
		return ERR_CAST(cxlrd);
	}

	cxled = cxl_request_dpa(cxlmd, CXL_PARTMODE_RAM, ram_size);
	if (IS_ERR(cxled)) {
		dev_err(&cxlmd->dev, "failed to request DPA: %ld\n",
			PTR_ERR(cxled));
		cxl_put_root_decoder(cxlrd);
		return ERR_CAST(cxled);
	}

	cxlr = cxl_create_region(cxlrd, &cxled, 1);
	cxl_put_root_decoder(cxlrd);
	if (IS_ERR(cxlr)) {
		dev_err(&cxlmd->dev, "failed to create region: %ld\n",
			PTR_ERR(cxlr));
		cxl_dpa_free(cxled);
		return cxlr;
	}

	ctx->cxled = cxled;
	dev_info(&cxlmd->dev, "created region %s\n",
		 dev_name(cxl_region_dev(cxlr)));
	return cxlr;
}

static int setup_private_node(struct cxl_memdev *cxlmd,
			      struct cxl_region *cxlr)
{
	struct cxl_mempolicy_ctx *ctx = memdev_to_ctx(cxlmd);
	struct range hpa_range;
	int rc;

	device_release_driver(cxl_region_dev(cxlr));

	rc = devm_cxl_add_sysram(cxlr, true, MMOP_ONLINE_MOVABLE);
	if (rc) {
		dev_err(cxl_region_dev(cxlr),
			"failed to add sysram: %d\n", rc);
		if (device_attach(cxl_region_dev(cxlr)) < 0)
			dev_warn(cxl_region_dev(cxlr),
				 "failed to re-attach driver\n");
		return rc;
	}

	rc = cxl_get_region_range(cxlr, &hpa_range);
	if (rc) {
		dev_err(cxl_region_dev(cxlr),
			"failed to get region range: %d\n", rc);
		return rc;
	}

	ctx->nid = phys_to_target_node(hpa_range.start);
	if (ctx->nid == NUMA_NO_NODE)
		ctx->nid = memory_add_physaddr_to_nid(hpa_range.start);

	rc = node_private_set_ops(ctx->nid, &cxl_mempolicy_ops);
	if (rc) {
		dev_err(cxl_region_dev(cxlr),
			"failed to set ops on node %d: %d\n", ctx->nid, rc);
		ctx->nid = NUMA_NO_NODE;
		return rc;
	}

	dev_info(&cxlmd->dev,
		 "node %d registered as private mempolicy memory\n", ctx->nid);
	return 0;
}

static int cxl_mempolicy_attach_dvsec(struct cxl_memdev *cxlmd)
{
	struct cxl_mempolicy_ctx *ctx = memdev_to_ctx(cxlmd);
	struct pci_dev *pdev = to_pci_dev(cxlmd->dev.parent);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_endpoint_dvsec_info *info = &cxlds->dvsec_info;
	int i, rc;

	dev_info(&cxlmd->dev,
		 "DVSEC fallback: %d range(s) available\n", info->ranges);

	for (i = 0; i < info->ranges; i++) {
		struct range *hpa = &info->dvsec_range[i];
		int nid;

		if (range_len(hpa) == 0)
			continue;

		dev_info(&cxlmd->dev,
			 "DVSEC fallback: adding range %d [%#llx-%#llx]\n",
			 i, (u64)hpa->start, (u64)hpa->end);

		rc = devm_cxl_add_sysram_range(&pdev->dev, hpa, true,
					       MMOP_ONLINE_MOVABLE);
		if (rc) {
			dev_err(&cxlmd->dev,
				"DVSEC range %d: failed to add sysram: %d\n",
				i, rc);
			return rc;
		}

		nid = phys_to_target_node(hpa->start);
		if (nid == NUMA_NO_NODE)
			nid = memory_add_physaddr_to_nid(hpa->start);

		rc = node_private_set_ops(nid, &cxl_mempolicy_ops);
		if (rc) {
			dev_err(&cxlmd->dev,
				"failed to set ops on node %d: %d\n", nid, rc);
			return rc;
		}

		ctx->nid = nid;
		dev_info(&cxlmd->dev,
			 "node %d registered as private mempolicy memory (DVSEC)\n",
			 nid);
	}

	return 0;
}

static int cxl_mempolicy_attach_probe(struct cxl_memdev *cxlmd)
{
	struct cxl_region *regions[8];
	struct cxl_region *cxlr;
	int nr, i;
	int rc;

	/* RCD fallback: no endpoint port, use DVSEC ranges directly */
	if (!cxlmd->endpoint)
		return cxl_mempolicy_attach_dvsec(cxlmd);

	dev_info(&cxlmd->dev,
		 "cxl_mempolicy attach: looking for regions\n");

	/* Phase 1: look for pre-committed RAM regions */
	nr = cxl_get_committed_regions(cxlmd, regions, ARRAY_SIZE(regions));
	for (i = 0; i < nr; i++) {
		if (cxl_region_mode(regions[i]) != CXL_PARTMODE_RAM) {
			put_device(cxl_region_dev(regions[i]));
			continue;
		}

		cxlr = regions[i];
		rc = setup_private_node(cxlmd, cxlr);
		put_device(cxl_region_dev(cxlr));
		if (rc == 0) {
			/* Release remaining region references */
			for (i++; i < nr; i++)
				put_device(cxl_region_dev(regions[i]));
			return 0;
		}
	}

	/* Phase 2: no committed regions, create one */
	dev_info(&cxlmd->dev,
		 "no existing regions, creating RAM region\n");

	cxlr = create_ram_region(cxlmd);
	if (IS_ERR(cxlr)) {
		rc = PTR_ERR(cxlr);
		if (rc == -ENODEV) {
			dev_info(&cxlmd->dev,
				 "no RAM capacity: %d\n", rc);
			return 0;
		}
		return rc;
	}

	rc = setup_private_node(cxlmd, cxlr);
	if (rc) {
		dev_err(&cxlmd->dev,
			"failed to setup private node: %d\n", rc);
		return rc;
	}

	/* Only take ownership of regions we created (Phase 2) */
	memdev_to_ctx(cxlmd)->cxlr = cxlr;

	return 0;
}

static const struct cxl_memdev_attach cxl_mempolicy_attach = {
	.probe = cxl_mempolicy_attach_probe,
};

static int cxl_mempolicy_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	struct cxl_mempolicy_ctx *ctx;
	struct cxl_memdev *cxlmd;
	int rc;

	dev_info(&pdev->dev, "cxl_mempolicy: probing device\n");

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->nid = NUMA_NO_NODE;

	rc = xa_insert(&ctx_xa, (unsigned long)pdev, ctx, GFP_KERNEL);
	if (rc)
		return rc;

	cxlmd = cxl_pci_type3_probe_init(pdev, &cxl_mempolicy_attach);
	if (IS_ERR(cxlmd)) {
		xa_erase(&ctx_xa, (unsigned long)pdev);
		return PTR_ERR(cxlmd);
	}

	dev_info(&pdev->dev, "cxl_mempolicy: probe complete\n");
	return 0;
}

static void cxl_mempolicy_remove(struct pci_dev *pdev)
{
	struct cxl_mempolicy_ctx *ctx = xa_erase(&ctx_xa, (unsigned long)pdev);

	dev_info(&pdev->dev, "cxl_mempolicy: removing device\n");

	if (!ctx)
		return;

	if (ctx->nid != NUMA_NO_NODE)
		WARN_ON(node_private_clear_ops(ctx->nid, &cxl_mempolicy_ops));

	if (ctx->cxlr) {
		cxl_destroy_region(ctx->cxlr);
		ctx->cxlr = NULL;
	}

	if (ctx->cxled) {
		cxl_dpa_free(ctx->cxled);
		ctx->cxled = NULL;
	}
}

static const struct pci_device_id cxl_mempolicy_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x0d93) },
	{ },
};
MODULE_DEVICE_TABLE(pci, cxl_mempolicy_pci_tbl);

static struct pci_driver cxl_mempolicy_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= cxl_mempolicy_pci_tbl,
	.probe		= cxl_mempolicy_probe,
	.remove		= cxl_mempolicy_remove,
	.driver	= {
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_pci_driver(cxl_mempolicy_driver);

MODULE_DESCRIPTION("CXL: Private Memory with Mempolicy Support");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS("CXL");
