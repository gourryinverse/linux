// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Meta Platforms, Inc. All rights reserved. */
/*
 * CXL Sysram Region - Intermediate device for kmem hotplug configuration
 *
 * This provides an intermediate device between cxl_region and cxl_dax_region
 * that allows users to configure memory hotplug parameters (like online_type)
 * before the underlying dax_region is created and memory is hotplugged.
 */

#include <linux/memory_hotplug.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

static void cxl_sysram_region_release(struct device *dev)
{
	struct cxl_sysram_region *cxlr_sysram = to_cxl_sysram_region(dev);

	kfree(cxlr_sysram);
}

static ssize_t online_type_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct cxl_sysram_region *cxlr_sysram = to_cxl_sysram_region(dev);

	switch (cxlr_sysram->online_type) {
	case MMOP_OFFLINE:
		return sysfs_emit(buf, "offline\n");
	case MMOP_ONLINE:
		return sysfs_emit(buf, "online\n");
	case MMOP_ONLINE_MOVABLE:
		return sysfs_emit(buf, "online_movable\n");
	default:
		return sysfs_emit(buf, "invalid\n");
	}
}

static ssize_t online_type_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t len)
{
	struct cxl_sysram_region *cxlr_sysram = to_cxl_sysram_region(dev);

	if (sysfs_streq(buf, "offline"))
		cxlr_sysram->online_type = MMOP_OFFLINE;
	else if (sysfs_streq(buf, "online"))
		cxlr_sysram->online_type = MMOP_ONLINE;
	else if (sysfs_streq(buf, "online_movable"))
		cxlr_sysram->online_type = MMOP_ONLINE_MOVABLE;
	else
		return -EINVAL;

	return len;
}

static DEVICE_ATTR_RW(online_type);

static struct attribute *cxl_sysram_region_attrs[] = {
	&dev_attr_online_type.attr,
	NULL,
};

static const struct attribute_group cxl_sysram_region_attribute_group = {
	.attrs = cxl_sysram_region_attrs,
};

static const struct attribute_group *cxl_sysram_region_attribute_groups[] = {
	&cxl_base_attribute_group,
	&cxl_sysram_region_attribute_group,
	NULL,
};

const struct device_type cxl_sysram_region_type = {
	.name = "cxl_sysram_region",
	.release = cxl_sysram_region_release,
	.groups = cxl_sysram_region_attribute_groups,
};

static bool is_cxl_sysram_region(struct device *dev)
{
	return dev->type == &cxl_sysram_region_type;
}

struct cxl_sysram_region *to_cxl_sysram_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, !is_cxl_sysram_region(dev),
			  "not a cxl_sysram_region device\n"))
		return NULL;
	return container_of(dev, struct cxl_sysram_region, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_sysram_region, "CXL");

static struct lock_class_key cxl_sysram_region_key;

static struct cxl_sysram_region *cxl_sysram_region_alloc(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_sysram_region *cxlr_sysram;
	struct device *dev;

	guard(rwsem_read)(&cxl_rwsem.region);
	if (p->state != CXL_CONFIG_COMMIT)
		return ERR_PTR(-ENXIO);

	cxlr_sysram = kzalloc(sizeof(*cxlr_sysram), GFP_KERNEL);
	if (!cxlr_sysram)
		return ERR_PTR(-ENOMEM);

	cxlr_sysram->hpa_range.start = p->res->start;
	cxlr_sysram->hpa_range.end = p->res->end;
	cxlr_sysram->online_type = -1;  /* Require explicit configuration */

	dev = &cxlr_sysram->dev;
	cxlr_sysram->cxlr = cxlr;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_sysram_region_key);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_sysram_region_type;

	return cxlr_sysram;
}

static void cxlr_sysram_unregister(void *_cxlr_sysram)
{
	struct cxl_sysram_region *cxlr_sysram = _cxlr_sysram;

	device_unregister(&cxlr_sysram->dev);
}

int devm_cxl_add_sysram_region(struct cxl_region *cxlr)
{
	struct cxl_sysram_region *cxlr_sysram;
	struct device *dev;
	int rc;

	cxlr_sysram = cxl_sysram_region_alloc(cxlr);
	if (IS_ERR(cxlr_sysram))
		return PTR_ERR(cxlr_sysram);

	dev = &cxlr_sysram->dev;
	rc = dev_set_name(dev, "sysram_region%d", cxlr->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	dev_dbg(&cxlr->dev, "%s: register %s\n", dev_name(dev->parent),
		dev_name(dev));

	return devm_add_action_or_reset(&cxlr->dev, cxlr_sysram_unregister,
					cxlr_sysram);
err:
	put_device(dev);
	return rc;
}

static int cxl_sysram_region_driver_probe(struct device *dev)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	/* Only handle RAM regions */
	if (cxlr->mode != CXL_PARTMODE_RAM)
		return -ENODEV;

	return devm_cxl_add_sysram_region(cxlr);
}

struct cxl_driver cxl_sysram_region_driver = {
	.name = "cxl_sysram_region",
	.probe = cxl_sysram_region_driver_probe,
	.id = CXL_DEVICE_REGION,
};
