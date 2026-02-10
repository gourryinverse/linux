// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Meta Platforms, Inc. All rights reserved. */
/*
 * CXL Sysram Region - Direct memory hotplug for CXL RAM regions
 *
 * This interface directly performs memory hotplug for CXL RAM regions,
 * eliminating the indirection through DAX.
 */

#include <linux/memory_hotplug.h>
#include <linux/memory-tiers.h>
#include <linux/memory.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

static const char *sysram_res_name = "System RAM (CXL)";

/**
 * cxl_region_find_sysram - Find the sysram device associated with a region
 * @cxlr: The CXL region
 *
 * Finds and returns the sysram child device of a CXL region.
 * The caller must release the device reference with put_device()
 * when done with the returned pointer.
 *
 * Return: Pointer to cxl_sysram, or NULL if not found
 */
struct cxl_sysram *cxl_region_find_sysram(struct cxl_region *cxlr)
{
	struct cxl_sysram *sysram;
	struct device *sdev;
	char sname[32];

	snprintf(sname, sizeof(sname), "sysram_region%d", cxlr->id);
	sdev = device_find_child_by_name(&cxlr->dev, sname);
	if (!sdev)
		return NULL;

	sysram = to_cxl_sysram(sdev);
	return sysram;
}
EXPORT_SYMBOL_NS_GPL(cxl_region_find_sysram, "CXL");

static int sysram_get_numa_node(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	int nid;

	nid = phys_to_target_node(p->res->start);
	if (nid == NUMA_NO_NODE)
		nid = memory_add_physaddr_to_nid(p->res->start);

	return nid;
}

static int sysram_hotplug_add(struct cxl_sysram *sysram, enum mmop online_type)
{
	struct resource *res;
	mhp_t mhp_flags;
	int rc;

	if (sysram->res)
		return -EBUSY;

	res = request_mem_region(sysram->hpa_range.start,
				 range_len(&sysram->hpa_range),
				 sysram->res_name);
	if (!res)
		return -EBUSY;

	sysram->res = res;

	/*
	 * Set flags appropriate for System RAM. Leave ..._BUSY clear
	 * so that add_memory() can add a child resource.
	 */
	res->flags = IORESOURCE_SYSTEM_RAM;

	mhp_flags = MHP_NID_IS_MGID;

	/*
	 * Ensure that future kexec'd kernels will not treat
	 * this as RAM automatically.
	 *
	 * For private regions, use add_private_memory_driver_managed()
	 * to register as a private node which isolates the memory from
	 * normal allocations and reclaim.
	 */
	rc = __add_memory_driver_managed(sysram->mgid,
					 sysram->hpa_range.start,
					 range_len(&sysram->hpa_range),
					 sysram_res_name, mhp_flags,
					 online_type,
					 sysram->private ? &sysram->np : NULL);
	if (rc) {
		remove_resource(res);
		kfree(res);
		sysram->res = NULL;
		return rc;
	}

	return 0;
}

static int sysram_hotplug_remove(struct cxl_sysram *sysram)
{
	int rc;

	if (!sysram->res)
		return 0;

	rc = offline_and_remove_memory(sysram->hpa_range.start,
				       range_len(&sysram->hpa_range));
	if (rc)
		return rc;

	if (sysram->res) {
		remove_resource(sysram->res);
		kfree(sysram->res);
		sysram->res = NULL;
	}

	return 0;
}

int cxl_sysram_offline_and_remove(struct cxl_sysram *sysram)
{
	return sysram_hotplug_remove(sysram);
}
EXPORT_SYMBOL_NS_GPL(cxl_sysram_offline_and_remove, "CXL");

static void cxl_sysram_release(struct device *dev)
{
	struct cxl_sysram *sysram = to_cxl_sysram(dev);

	if (sysram->res)
		sysram_hotplug_remove(sysram);

	kfree(sysram->res_name);

	if (sysram->mgid >= 0)
		memory_group_unregister(sysram->mgid);

	if (sysram->mtype)
		clear_node_memory_type(sysram->numa_node, sysram->mtype);

	kfree(sysram);
}

static ssize_t hotplug_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct cxl_sysram *sysram = to_cxl_sysram(dev);
	int online_type, rc;

	online_type = mhp_online_type_from_str(buf);
	if (online_type < 0)
		return online_type;

	if (online_type == MMOP_OFFLINE)
		rc = sysram_hotplug_remove(sysram);
	else
		rc = sysram_hotplug_add(sysram, online_type);

	if (rc)
		dev_warn(dev, "hotplug %s failed: %d\n",
			 online_type == MMOP_OFFLINE ? "offline" : "online", rc);

	return rc ? rc : len;
}
static DEVICE_ATTR_WO(hotplug);

static struct attribute *cxl_sysram_attrs[] = {
	&dev_attr_hotplug.attr,
	NULL
};

static const struct attribute_group cxl_sysram_attribute_group = {
	.attrs = cxl_sysram_attrs,
};

static const struct attribute_group *cxl_sysram_attribute_groups[] = {
	&cxl_base_attribute_group,
	&cxl_sysram_attribute_group,
	NULL
};

const struct device_type cxl_sysram_type = {
	.name = "cxl_sysram",
	.release = cxl_sysram_release,
	.groups = cxl_sysram_attribute_groups,
};

static bool is_cxl_sysram(struct device *dev)
{
	return dev->type == &cxl_sysram_type;
}

struct cxl_sysram *to_cxl_sysram(struct device *dev)
{
	if (dev_WARN_ONCE(dev, !is_cxl_sysram(dev),
			  "not a cxl_sysram device\n"))
		return NULL;
	return container_of(dev, struct cxl_sysram, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_sysram, "CXL");

struct device *cxl_sysram_dev(struct cxl_sysram *sysram)
{
	return &sysram->dev;
}
EXPORT_SYMBOL_NS_GPL(cxl_sysram_dev, "CXL");

static struct lock_class_key cxl_sysram_key;

static enum mmop cxl_sysram_get_default_online_type(void)
{
	if (IS_ENABLED(CONFIG_CXL_SYSRAM_ONLINE_TYPE_SYSTEM_DEFAULT))
		return mhp_get_default_online_type();
	if (IS_ENABLED(CONFIG_CXL_SYSRAM_ONLINE_TYPE_MOVABLE))
		return MMOP_ONLINE_MOVABLE;
	if (IS_ENABLED(CONFIG_CXL_SYSRAM_ONLINE_TYPE_NORMAL))
		return MMOP_ONLINE;
	return MMOP_OFFLINE;
}

static struct cxl_sysram *cxl_sysram_alloc(struct cxl_region *cxlr)
{
	struct cxl_sysram *sysram __free(kfree) = NULL;
	struct device *dev;

	sysram = kzalloc(sizeof(*sysram), GFP_KERNEL);
	if (!sysram)
		return ERR_PTR(-ENOMEM);

	sysram->online_type = cxl_sysram_get_default_online_type();
	sysram->last_hotplug_cmd = MMOP_OFFLINE;
	sysram->numa_node = -1;
	sysram->mgid = -1;

	dev = &sysram->dev;
	sysram->cxlr = cxlr;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_sysram_key);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_sysram_type;

	return_ptr(sysram);
}

static void sysram_unregister(void *_sysram)
{
	struct cxl_sysram *sysram = _sysram;

	device_unregister(&sysram->dev);
}

int devm_cxl_add_sysram(struct cxl_region *cxlr, bool private,
			enum mmop online_type)
{
	struct cxl_sysram *sysram __free(put_cxl_sysram) = NULL;
	struct memory_dev_type *mtype;
	struct range hpa_range;
	struct device *dev;
	int adist = MEMTIER_DEFAULT_LOWTIER_ADISTANCE;
	int numa_node;
	int rc;

	rc = cxl_region_get_hpa_range(cxlr, &hpa_range);
	if (rc)
		return rc;

	hpa_range = memory_block_align_range(&hpa_range);
	if (hpa_range.start >= hpa_range.end) {
		dev_warn(&cxlr->dev, "region too small after alignment\n");
		return -ENOSPC;
	}

	sysram = cxl_sysram_alloc(cxlr);
	if (IS_ERR(sysram))
		return PTR_ERR(sysram);

	sysram->hpa_range = hpa_range;

	sysram->res_name = kasprintf(GFP_KERNEL, "cxl_sysram%d", cxlr->id);
	if (!sysram->res_name)
		return -ENOMEM;

	/* Override default online type if caller specified one */
	if (online_type >= 0)
		sysram->online_type = online_type;

	/* Set up private node registration if requested */
	sysram->private = private;
	if (private)
		sysram->np.owner = sysram;

	dev = &sysram->dev;

	rc = dev_set_name(dev, "sysram_region%d", cxlr->id);
	if (rc)
		return rc;

	/* Setup memory tier before adding device */
	numa_node = sysram_get_numa_node(cxlr);
	if (numa_node < 0) {
		dev_warn(&cxlr->dev, "rejecting region with invalid node: %d\n",
			 numa_node);
		return -EINVAL;
	}
	sysram->numa_node = numa_node;

	mt_calc_adistance(numa_node, &adist);
	mtype = mt_get_memory_type(adist);
	if (IS_ERR(mtype))
		return PTR_ERR(mtype);
	sysram->mtype = mtype;

	init_node_memory_type(numa_node, mtype);

	/* Register memory group for this region */
	rc = memory_group_register_static(numa_node,
					  PFN_UP(range_len(&hpa_range)));
	if (rc < 0)
		return rc;
	sysram->mgid = rc;

	rc = device_add(dev);
	if (rc)
		return rc;

	dev_dbg(&cxlr->dev, "%s: register %s\n", dev_name(dev->parent),
		dev_name(dev));

	/*
	 * Dynamic capacity regions (DCD) will have memory added later.
	 * For static RAM regions, hotplug the entire range now.
	 */
	if (cxlr->mode != CXL_PARTMODE_RAM)
		goto out;

	/* If default online_type is a valid online mode, immediately hotplug */
	if (sysram->online_type > MMOP_OFFLINE) {
		rc = sysram_hotplug_add(sysram, sysram->online_type);
		if (rc)
			dev_warn(dev, "hotplug failed: %d\n", rc);
		else
			sysram->last_hotplug_cmd = sysram->online_type;
	}

out:
	return devm_add_action_or_reset(&cxlr->dev, sysram_unregister,
					no_free_ptr(sysram));
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_sysram, "CXL");
