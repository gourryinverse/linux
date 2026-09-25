// SPDX-License-Identifier: GPL-2.0
/* Explicit device-dax test provider for compressed anonymous memory. */

#include <linux/atomic.h>
#include <linux/cram.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nodemask.h>
#include <linux/range.h>
#include <linux/slab.h>

#include "bus.h"
#include "dax-private.h"

struct cramdax_data {
	struct mutex lock;		/* serialize state and configuration */
	bool online;
	u32 zratio;
	unsigned long features;
	struct range *ranges;
	unsigned int nr_ranges;
	unsigned long nr_pages;
	atomic_long_t trim_pages;
};

static int cramdax_range(struct dev_dax *dev_dax, int id, struct range *range)
{
	struct range *dax_range = &dev_dax->ranges[id].range;

	*range = memory_block_aligned_range(dax_range);
	if (range->start >= range->end)
		return -ENOSPC;
	return 0;
}

static struct range *cramdax_ranges(struct dev_dax *dev_dax,
				    unsigned int *nr_ranges)
{
	struct range *ranges;
	unsigned int nr = 0;
	int i;

	ranges = kmalloc_array(dev_dax->nr_range, sizeof(*ranges), GFP_KERNEL);
	if (!ranges)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < dev_dax->nr_range; i++)
		if (!cramdax_range(dev_dax, i, &ranges[nr]))
			nr++;
	if (!nr) {
		kfree(ranges);
		return ERR_PTR(-ENOSPC);
	}
	*nr_ranges = nr;
	return ranges;
}

static int cramdax_trim(void *driver_data, unsigned long start_pfn,
			unsigned long nr_pages)
{
	struct cramdax_data *data = driver_data;
	unsigned long i;

	for (i = 0; i < nr_pages; i++)
		clear_highpage(pfn_to_page(start_pfn + i));
	atomic_long_add(nr_pages, &data->trim_pages);
	return 0;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct cramdax_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", READ_ONCE(data->online) ?
			  "online" : "offline");
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct dev_dax *dev_dax = to_dev_dax(dev);
	struct cramdax_data *data = dev_get_drvdata(dev);
	bool online;
	int ret;

	if (sysfs_streq(buf, "online"))
		online = true;
	else if (sysfs_streq(buf, "offline") || sysfs_streq(buf, "unplugged"))
		online = false;
	else
		return -EINVAL;

	guard(mutex)(&data->lock);
	if (online == data->online)
		return len;

	if (online) {
		struct cram_ops ops = {
			.owner = THIS_MODULE,
			.trim = cramdax_trim,
		};

		ret = cram_register(dev_dax->target_node, data->ranges,
				    data->nr_ranges, data->features, ops, data);
	} else {
		ret = cram_unregister(dev_dax->target_node, data->ranges,
				      data->nr_ranges);
	}
	if (ret)
		return ret;

	data->online = online;
	return len;
}
static DEVICE_ATTR_RW(state);

static ssize_t memory_features_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cramdax_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%#lx\n", data->features);
}

static ssize_t memory_features_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct cramdax_data *data = dev_get_drvdata(dev);
	unsigned long features;
	int ret;

	ret = kstrtoul(buf, 0, &features);
	if (ret)
		return ret;
	if (!(features & NODE_MEMORY_FEAT_RECLAIM) ||
	    !(features & NODE_MEMORY_FEAT_WR_FENCE) ||
	    (features & NODE_MEMORY_FEAT_COMMON) ||
	    (features & ~NODE_MEMORY_FEAT_VALID))
		return -EINVAL;

	guard(mutex)(&data->lock);
	if (data->online)
		return -EBUSY;
	data->features = features;
	return len;
}
static DEVICE_ATTR_RW(memory_features);

static ssize_t zratio_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct cramdax_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->zratio);
}

static ssize_t zratio_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct cramdax_data *data = dev_get_drvdata(dev);
	u32 ratio;
	int ret;

	ret = kstrtou32(buf, 0, &ratio);
	if (ret)
		return ret;
	if (ratio < 1000)
		return -EINVAL;

	guard(mutex)(&data->lock);
	if (data->online)
		return -EBUSY;
	data->zratio = ratio;
	return len;
}
static DEVICE_ATTR_RW(zratio);

static ssize_t balloon_target_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct dev_dax *dev_dax = to_dev_dax(dev);
	struct cramdax_data *data = dev_get_drvdata(dev);
	unsigned long target, capacity;
	int ret;

	ret = kstrtoul(buf, 0, &target);
	if (ret)
		return ret;

	guard(mutex)(&data->lock);
	if (!data->online)
		return -ENODEV;
	capacity = data->nr_pages - min(target, data->nr_pages);
	ret = cram_set_capacity(dev_dax->target_node, capacity);
	return ret ?: len;
}
static DEVICE_ATTR_WO(balloon_target);

static ssize_t compression_ratio_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct dev_dax *dev_dax = to_dev_dax(dev);
	struct cramdax_data *data = dev_get_drvdata(dev);
	unsigned long capacity;
	u32 ratio;
	int ret;

	ret = kstrtou32(buf, 0, &ratio);
	if (ret)
		return ret;
	if (ratio < 1000)
		return -EINVAL;

	guard(mutex)(&data->lock);
	if (!data->online)
		return -ENODEV;
	capacity = data->nr_pages;
	if (ratio < data->zratio)
		capacity = mul_u64_u32_div(capacity, ratio, data->zratio);
	ret = cram_set_capacity(dev_dax->target_node, capacity);
	return ret ?: len;
}
static DEVICE_ATTR_WO(compression_ratio);

static ssize_t no_alloc_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t len)
{
	struct dev_dax *dev_dax = to_dev_dax(dev);
	struct cramdax_data *data = dev_get_drvdata(dev);
	bool no_alloc;
	int ret;

	ret = kstrtobool(buf, &no_alloc);
	if (ret)
		return ret;

	guard(mutex)(&data->lock);
	if (!data->online)
		return -ENODEV;
	ret = cram_set_no_alloc(dev_dax->target_node, no_alloc);
	return ret ?: len;
}
static DEVICE_ATTR_WO(no_alloc);

static ssize_t trim_count_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct cramdax_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%ld\n", atomic_long_read(&data->trim_pages));
}
static DEVICE_ATTR_RO(trim_count);

static int cramdax_probe(struct dev_dax *dev_dax)
{
	struct cramdax_data *data;
	unsigned int i;

	if (dev_dax->target_node < 0)
		return -EINVAL;
	data = kzalloc_obj(*data);
	if (!data)
		return -ENOMEM;
	data->ranges = cramdax_ranges(dev_dax, &data->nr_ranges);
	if (IS_ERR(data->ranges)) {
		int ret = PTR_ERR(data->ranges);

		kfree(data);
		return ret;
	}
	for (i = 0; i < data->nr_ranges; i++)
		data->nr_pages += range_len(&data->ranges[i]) >> PAGE_SHIFT;
	mutex_init(&data->lock);
	data->zratio = 1000;
	data->features = NODE_MEMORY_FEAT_RECLAIM |
			 NODE_MEMORY_FEAT_WR_FENCE;
	dev_set_drvdata(&dev_dax->dev, data);
	return 0;
}

static void cramdax_remove(struct dev_dax *dev_dax)
{
	struct device *dev = &dev_dax->dev;
	struct cramdax_data *data = dev_get_drvdata(dev);
	if (!data->online)
		goto free;

	if (cram_unregister(dev_dax->target_node, data->ranges,
			    data->nr_ranges)) {
		dev_err(dev, "CRAM memory stuck online until reboot\n");
		return;
	}
free:
	dev_set_drvdata(dev, NULL);
	kfree(data->ranges);
	kfree(data);
}

static struct attribute *cramdax_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_memory_features.attr,
	&dev_attr_zratio.attr,
	&dev_attr_balloon_target.attr,
	&dev_attr_compression_ratio.attr,
	&dev_attr_no_alloc.attr,
	&dev_attr_trim_count.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cramdax);

static struct dax_device_driver cramdax_driver = {
	.probe = cramdax_probe,
	.remove = cramdax_remove,
	.type = DAXDRV_CRAM_TYPE,
	.drv = {
		.dev_groups = cramdax_groups,
	},
};

static int __init cramdax_init(void)
{
	return dax_driver_register(&cramdax_driver);
}

static void __exit cramdax_exit(void)
{
	dax_driver_unregister(&cramdax_driver);
}

module_init(cramdax_init);
module_exit(cramdax_exit);
MODULE_AUTHOR("Gregory Price <gourry@gourry.net>");
MODULE_DESCRIPTION("device-dax test provider for CRAM");
MODULE_LICENSE("GPL");
