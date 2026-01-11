// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Meta Inc. All rights reserved. */
#include <linux/memremap.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/memory-tiers.h>
#include <linux/memory_hotplug.h>
#include <linux/string_helpers.h>
#include <linux/sched/signal.h>
#include <cxlmem.h>
#include <cxl.h>
#include "../core.h"

/* If HMAT was unavailable, assign a default distance. */
#define MEMTIER_DEFAULT_CXL_ADISTANCE	(MEMTIER_ADISTANCE_DRAM * 5)

static const char *sysram_name = "System RAM (CXL)";

struct cxl_sysram_data {
	const char *res_name;
	int mgid;
	struct resource *res;
};

static DEFINE_MUTEX(cxl_memory_type_lock);
static LIST_HEAD(cxl_memory_types);

static struct cxl_region *to_cxl_region(struct device *dev)
{
	if (dev->type != &cxl_region_type)
		return NULL;
	return container_of(dev, struct cxl_region, dev);
}

static struct memory_dev_type *cxl_find_alloc_memory_type(int adist)
{
	guard(mutex)(&cxl_memory_type_lock);
	return mt_find_alloc_memory_type(adist, &cxl_memory_types);
}

static void __maybe_unused cxl_put_memory_types(void)
{
	guard(mutex)(&cxl_memory_type_lock);
	mt_put_memory_types(&cxl_memory_types);
}

static int cxl_sysram_range(struct cxl_region *cxlr, struct range *r)
{
	struct cxl_region_params *p = &cxlr->params;

	if (!p->res)
		return -ENODEV;

	/* memory-block align the hotplug range */
	r->start = ALIGN(p->res->start, memory_block_size_bytes());
	r->end = ALIGN_DOWN(p->res->end + 1, memory_block_size_bytes()) - 1;
	if (r->start >= r->end) {
		r->start = p->res->start;
		r->end = p->res->end;
		return -ENOSPC;
	}
	return 0;
}

static ssize_t hotunplug_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct range range;
	int rc;

	if (!cxlr)
		return -ENODEV;

	rc = cxl_sysram_range(cxlr, &range);
	if (rc)
		return rc;

	rc = offline_and_remove_memory(range.start, range_len(&range));

	if (rc)
		return rc;

	return len;
}
static DEVICE_ATTR_WO(hotunplug);

struct online_memory_cb_arg {
	int online_type;
	int rc;
};

static int online_memory_block_cb(struct memory_block *mem, void *arg)
{
	struct online_memory_cb_arg *cb_arg = arg;

	if (signal_pending(current))
		return -EINTR;

	cond_resched();

	if (mem->state == MEM_ONLINE)
		return 0;

	mem->online_type = cb_arg->online_type;
	cb_arg->rc = device_online(&mem->dev);

	return cb_arg->rc;
}

static int offline_memory_block_cb(struct memory_block *mem, void *arg)
{
	int *rc = arg;

	if (signal_pending(current))
		return -EINTR;

	cond_resched();

	if (mem->state == MEM_OFFLINE)
		return 0;

	*rc = device_offline(&mem->dev);

	return *rc;
}

static ssize_t state_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct online_memory_cb_arg cb_arg;
	struct range range;
	int rc;

	if (!cxlr)
		return -ENODEV;

	rc = cxl_sysram_range(cxlr, &range);
	if (rc)
		return rc;

	rc = lock_device_hotplug_sysfs();
	if (rc)
		return rc;

	if (sysfs_streq(buf, "online")) {
		cb_arg.online_type = MMOP_ONLINE_MOVABLE;
		cb_arg.rc = 0;
		rc = walk_memory_blocks(range.start, range_len(&range),
					&cb_arg, online_memory_block_cb);
		if (!rc)
			rc = cb_arg.rc;
	} else if (sysfs_streq(buf, "online_normal")) {
		cb_arg.online_type = MMOP_ONLINE;
		cb_arg.rc = 0;
		rc = walk_memory_blocks(range.start, range_len(&range),
					&cb_arg, online_memory_block_cb);
		if (!rc)
			rc = cb_arg.rc;
	} else if (sysfs_streq(buf, "offline")) {
		int offline_rc = 0;

		rc = walk_memory_blocks(range.start, range_len(&range),
					&offline_rc, offline_memory_block_cb);
		if (!rc)
			rc = offline_rc;
	} else {
		rc = -EINVAL;
	}

	unlock_device_hotplug();

	if (rc)
		return rc;

	return len;
}
static DEVICE_ATTR_WO(state);

static ssize_t hotplug_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_sysram_data *data;
	struct range range;
	int rc;

	if (!cxlr)
		return -ENODEV;

	data = dev_get_drvdata(dev);
	if (!data)
		return -ENODEV;

	rc = cxl_sysram_range(cxlr, &range);
	if (rc)
		return rc;

	rc = add_memory_driver_managed(data->mgid, range.start,
				       range_len(&range), sysram_name,
				       MHP_NID_IS_MGID);
	if (rc)
		return rc;

	return len;
}
static DEVICE_ATTR_WO(hotplug);

static struct attribute *cxl_sysram_region_attrs[] = {
	&dev_attr_hotunplug.attr,
	&dev_attr_state.attr,
	&dev_attr_hotplug.attr,
	NULL,
};

static const struct attribute_group cxl_sysram_region_group = {
	.name = "memctl",
	.attrs = cxl_sysram_region_attrs,
};

static void cxl_sysram_unregister(void *_data)
{
	struct cxl_sysram_data *data = _data;
	struct range range = {
		.start = data->res->start,
		.end = data->res->end
	};

	/* We have one shot for removal, otherwise it's stuck til reboot */
	if (!offline_and_remove_memory(range.start, range_len(&range))) {
		remove_resource(data->res);
		kfree(data->res);
		memory_group_unregister(data->mgid);
		kfree(data->res_name);
		kfree(data);
		return;
	}
	pr_err("CXL: %#llx-%#llx cannot be hotremoved until next reboot\n",
	       range.start, range.end);
}

int devm_cxl_add_sysram_region(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct device *dev = &cxlr->dev;
	struct cxl_sysram_data *data;
	struct memory_dev_type *mtype;
	unsigned long total_len = 0;
	struct resource *res;
	struct range range;
	mhp_t mhp_flags;
	int numa_node;
	int adist = MEMTIER_DEFAULT_CXL_ADISTANCE;
	int rc;

	numa_node = phys_to_target_node(p->res->start);
	if (numa_node < 0) {
		dev_warn(dev, "rejecting CXL region with invalid node: %d\n",
			 numa_node);
		return -EINVAL;
	}

	rc = cxl_sysram_range(cxlr, &range);
	if (rc) {
		dev_info(dev, "range %#llx-%#llx too small after alignment\n",
			 range.start, range.end);
		return rc;
	}
	total_len = range_len(&range);

	if (!total_len) {
		dev_warn(dev, "rejecting CXL region without any memory after alignment\n");
		return -EINVAL;
	}

	mt_calc_adistance(numa_node, &adist);
	mtype = cxl_find_alloc_memory_type(adist);
	if (IS_ERR(mtype))
		return PTR_ERR(mtype);

	init_node_memory_type(numa_node, mtype);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		rc = -ENOMEM;
		goto err_data;
	}

	data->res_name = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!data->res_name) {
		rc = -ENOMEM;
		goto err_res_name;
	}

	rc = memory_group_register_static(numa_node, PFN_UP(total_len));
	if (rc < 0)
		goto err_reg_mgid;
	data->mgid = rc;

	/* Region is permanently reserved if hotremove fails when unbinding. */
	res = request_mem_region(range.start, range_len(&range),
				 data->res_name);
	if (!res) {
		dev_warn(dev, "range %#llx-%#llx could not reserve region\n",
			 range.start, range.end);
		rc = -EBUSY;
		goto err_request_mem;
	}
	data->res = res;

	/*
	 * Setup flags for System RAM. Leave _BUSY clear so add_memory() can add
	 * a child resource. Do not inherit flags from parent since it may set
	 * flags unknown to us that will the break add_memory() below.
	 */
	res->flags = IORESOURCE_SYSTEM_RAM;
	mhp_flags = MHP_NID_IS_MGID;
	rc = add_memory_driver_managed(data->mgid, range.start,
				       range_len(&range), sysram_name, mhp_flags);
	if (rc) {
		dev_warn(dev, "range %#llx-%#llx memory add failed\n",
			 range.start, range.end);
		goto err_add_memory;
	}
	dev_dbg(dev, "%s: added %llu bytes as System RAM\n", dev_name(dev),
		(unsigned long long)total_len);

	dev_set_drvdata(dev, data);
	rc = devm_device_add_group(dev, &cxl_sysram_region_group);
	if (rc)
		goto err_add_group;

	return devm_add_action_or_reset(dev, cxl_sysram_unregister, data);

err_add_group:
	dev_set_drvdata(dev, NULL);
	/* if this fails, memory cannot be removed from the system until reboot */
	remove_memory(range.start, range_len(&range));
err_add_memory:
	remove_resource(res);
	kfree(res);
err_request_mem:
	memory_group_unregister(data->mgid);
err_reg_mgid:
	kfree(data->res_name);
err_res_name:
	kfree(data);
err_data:
	clear_node_memory_type(numa_node, mtype);
	return rc;
}
