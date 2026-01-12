// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
/* Copyright(c) 2026 Meta Inc. All rights reserved. */
#include <linux/device.h>
#include <linux/ioport.h>
#include <cxlmem.h>
#include <cxl.h>
#include "../core.h"

static int is_system_ram(struct resource *res, void *arg)
{
	struct cxl_region *cxlr = arg;
	struct cxl_region_params *p = &cxlr->params;

	dev_dbg(&cxlr->dev, "%pr has System RAM: %pr\n", p->res, res);
	return 1;
}

int cxl_enable_memctrl(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;

	switch (cxlr->memctrl) {
	case CXL_MEMCTRL_AUTO:
		/*
		 * The region can not be manged by CXL if any portion of
		 * it is already online as 'System RAM'
		 */
		if (walk_iomem_res_desc(IORES_DESC_NONE,
					IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY,
					p->res->start, p->res->end, cxlr,
					is_system_ram) > 0)
			return 0;
		return devm_cxl_add_dax_region(cxlr);
	case CXL_MEMCTRL_DAX:
		return devm_cxl_add_dax_region(cxlr);
	case CXL_MEMCTRL_SYSRAM:
		return devm_cxl_add_sysram_region(cxlr);
	case CXL_MEMCTRL_PMEM:
		return devm_cxl_add_pmem_region(cxlr);
	default:
		return -EINVAL;
	}
}


