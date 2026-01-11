// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/device.h>
#include <linux/slab.h>
#include <cxlmem.h>
#include <cxl.h>
#include "../core.h"

static struct lock_class_key cxl_dax_region_key;

static struct cxl_dax_region *cxl_dax_region_alloc(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_dax_region *cxlr_dax;
	struct device *dev;

	guard(rwsem_read)(&cxl_rwsem.region);
	if (p->state != CXL_CONFIG_COMMIT)
		return ERR_PTR(-ENXIO);

	cxlr_dax = kzalloc(sizeof(*cxlr_dax), GFP_KERNEL);
	if (!cxlr_dax)
		return ERR_PTR(-ENOMEM);

	cxlr_dax->hpa_range.start = p->res->start;
	cxlr_dax->hpa_range.end = p->res->end;

	dev = &cxlr_dax->dev;
	cxlr_dax->cxlr = cxlr;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_dax_region_key);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_dax_region_type;

	return cxlr_dax;
}

static void cxlr_dax_unregister(void *_cxlr_dax)
{
	struct cxl_dax_region *cxlr_dax = _cxlr_dax;

	device_unregister(&cxlr_dax->dev);
}

/*
 * The dax controller is the default controller and simply hands the
 * control pattern over to the dax driver.  It does with a dax_region
 * built by dax/cxl.c
 */
int devm_cxl_add_dax_region(struct cxl_region *cxlr)
{
	struct cxl_dax_region *cxlr_dax;
	struct device *dev;
	int rc;

	cxlr_dax = cxl_dax_region_alloc(cxlr);
	if (IS_ERR(cxlr_dax))
		return PTR_ERR(cxlr_dax);

	dev = &cxlr_dax->dev;
	rc = dev_set_name(dev, "dax_region%d", cxlr->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	dev_dbg(&cxlr->dev, "%s: register %s\n", dev_name(dev->parent),
		dev_name(dev));

	return devm_add_action_or_reset(&cxlr->dev, cxlr_dax_unregister,
					cxlr_dax);
err:
	put_device(dev);
	return rc;
}
