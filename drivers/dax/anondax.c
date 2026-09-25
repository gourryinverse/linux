// SPDX-License-Identifier: GPL-2.0
/* Anonymous shared mappings backed by explicitly allocated private memory. */

#include <linux/cdev.h>
#include <linux/dax.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nodemask.h>
#include <linux/range.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "bus.h"
#include "dax-private.h"

struct anondax_data {
	struct range *ranges;
	unsigned int nr_ranges;
	unsigned long nr_pages;
};

/* One shared allocation object per open file. */
struct anondax_file {
	struct mutex lock;		/* protects pages */
	struct xarray pages;
	unsigned long nr_pages;
	int nid;
};

static const char *anondax_name;
static bool any_hotremove_failed;

static int anondax_range(struct dev_dax *dev_dax, int id,
			 struct range *range)
{
	struct range *dax_range = &dev_dax->ranges[id].range;

	*range = memory_block_aligned_range(dax_range);
	if (range->start >= range->end)
		return -ENOSPC;
	return 0;
}

static struct range *anondax_ranges(struct dev_dax *dev_dax,
				    unsigned int *nr_ranges,
				    unsigned long *nr_pages)
{
	struct range *ranges;
	unsigned int nr = 0;
	int i;

	ranges = kmalloc_array(dev_dax->nr_range, sizeof(*ranges), GFP_KERNEL);
	if (!ranges)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < dev_dax->nr_range; i++) {
		if (anondax_range(dev_dax, i, &ranges[nr]))
			continue;
		*nr_pages += range_len(&ranges[nr]) >> PAGE_SHIFT;
		nr++;
	}
	if (!nr) {
		kfree(ranges);
		return ERR_PTR(-ENOSPC);
	}

	*nr_ranges = nr;
	return ranges;
}

static int anondax_add_memory(struct dev_dax *dev_dax,
			      struct anondax_data *data)
{
	unsigned int i;
	int ret;

	for (i = 0; i < data->nr_ranges; i++) {
		struct range *range = &data->ranges[i];

		ret = __add_memory_driver_managed(dev_dax->target_node,
						  range->start, range_len(range),
						  anondax_name, MHP_MERGE_RESOURCE,
						  MMOP_ONLINE_KERNEL,
						  NODE_MEMORY_FEAT_RECLAIM |
						  NODE_MEMORY_FEAT_COMPACTION);
		if (ret) {
			if (i && offline_and_remove_memory_ranges(data->ranges, i)) {
				dev_err(&dev_dax->dev,
					"private memory stuck online until reboot\n");
				any_hotremove_failed = true;
			}
			return ret;
		}
	}
	return 0;
}

static void anondax_file_free(struct anondax_file *afile)
{
	struct folio *folio;
	unsigned long index;

	xa_for_each(&afile->pages, index, folio)
		folio_put(folio);
	xa_destroy(&afile->pages);
	mutex_destroy(&afile->lock);
	kfree(afile);
}

static int anondax_open(struct inode *inode, struct file *file)
{
	struct dax_device *dax_dev = inode_dax(inode);
	struct dev_dax *dev_dax = dax_get_private(dax_dev);
	struct anondax_data *data;
	struct anondax_file *afile;
	int id, ret = 0;

	afile = kzalloc_obj(*afile);
	if (!afile)
		return -ENOMEM;

	id = dax_read_lock();
	if (!dax_alive(dax_dev)) {
		ret = -ENXIO;
		goto unlock;
	}
	data = dev_get_drvdata(&dev_dax->dev);
	if (!data) {
		ret = -ENXIO;
		goto unlock;
	}
	afile->nid = dev_dax->target_node;
	afile->nr_pages = data->nr_pages;
unlock:
	dax_read_unlock(id);
	if (ret) {
		kfree(afile);
		return ret;
	}

	mutex_init(&afile->lock);
	xa_init(&afile->pages);
	file->private_data = afile;
	return 0;
}

static int anondax_release(struct inode *inode, struct file *file)
{
	anondax_file_free(file->private_data);
	return 0;
}

static vm_fault_t anondax_fault(struct vm_fault *vmf)
{
	struct anondax_file *afile = vmf->vma->vm_private_data;
	struct dax_device *dax_dev = inode_dax(file_inode(vmf->vma->vm_file));
	struct folio *folio;
	vm_fault_t ret;
	void *old;
	int err, id;

	if (vmf->pgoff >= afile->nr_pages)
		return VM_FAULT_SIGBUS;

	id = dax_read_lock();
	if (!dax_alive(dax_dev)) {
		ret = VM_FAULT_SIGBUS;
		goto dax_unlock;
	}

	mutex_lock(&afile->lock);
	folio = xa_load(&afile->pages, vmf->pgoff);
	if (!folio) {
		if (!node_state(afile->nid, N_MEMORY) ||
		    node_state(afile->nid, N_MEMORY_COMMON)) {
			err = -ENXIO;
			goto unlock;
		}

		folio = folio_alloc_node_private(GFP_KERNEL | __GFP_THISNODE |
				__GFP_NORETRY | __GFP_NOWARN, 0, afile->nid);
		if (!folio) {
			err = -ENOMEM;
			goto unlock;
		}
		clear_user_highpage(folio_page(folio, 0), 0);

		old = xa_store(&afile->pages, vmf->pgoff, folio, GFP_KERNEL);
		err = xa_err(old);
		if (err) {
			folio_put(folio);
			goto unlock;
		}
		VM_WARN_ON_ONCE(old);
	}
	mutex_unlock(&afile->lock);

	ret = vmf_insert_page(vmf->vma, vmf->address,
			      folio_page(folio, 0));
	goto dax_unlock;

unlock:
	mutex_unlock(&afile->lock);
	ret = vmf_error(err);
dax_unlock:
	dax_read_unlock(id);
	return ret;
}

static const struct vm_operations_struct anondax_vm_ops = {
	.fault = anondax_fault,
};

static int anondax_mmap_prepare(struct vm_area_desc *desc)
{
	struct anondax_file *afile = desc->file->private_data;
	unsigned long nr_pages = vma_desc_pages(desc);

	if (!vma_desc_test(desc, VMA_SHARED_BIT))
		return -EINVAL;
	if (desc->pgoff >= afile->nr_pages ||
	    nr_pages > afile->nr_pages - desc->pgoff)
		return -EINVAL;

	vma_desc_set_flags(desc, VMA_MIXEDMAP_BIT, VMA_DONTEXPAND_BIT,
			   VMA_DONTDUMP_BIT);
	desc->vm_ops = &anondax_vm_ops;
	desc->private_data = afile;
	return 0;
}

static const struct file_operations anondax_fops = {
	.owner = THIS_MODULE,
	.open = anondax_open,
	.release = anondax_release,
	.mmap_prepare = anondax_mmap_prepare,
	.llseek = noop_llseek,
};

static int anondax_probe(struct dev_dax *dev_dax)
{
	struct dax_device *dax_dev = dev_dax->dax_dev;
	struct device *dev = &dev_dax->dev;
	struct anondax_data *data;
	struct inode *inode;
	struct cdev *cdev;
	int ret;

	if (dev_dax->target_node < 0)
		return -EINVAL;

	data = kzalloc_obj(*data);
	if (!data)
		return -ENOMEM;
	data->ranges = anondax_ranges(dev_dax, &data->nr_ranges,
				      &data->nr_pages);
	if (IS_ERR(data->ranges)) {
		ret = PTR_ERR(data->ranges);
		goto free_data;
	}

	ret = anondax_add_memory(dev_dax, data);
	if (ret)
		goto free_ranges;

	dev_set_drvdata(dev, data);
	inode = dax_inode(dax_dev);
	cdev = inode->i_cdev;
	cdev_init(cdev, &anondax_fops);
	cdev->owner = dev->driver->owner;
	cdev_set_parent(cdev, &dev->kobj);
	ret = cdev_add(cdev, dev->devt, 1);
	if (ret)
		goto remove_memory;

	run_dax(dax_dev);
	return 0;

remove_memory:
	dev_set_drvdata(dev, NULL);
	if (offline_and_remove_memory_ranges(data->ranges, data->nr_ranges)) {
		dev_err(dev, "private memory stuck online until reboot\n");
		any_hotremove_failed = true;
	}
free_ranges:
	kfree(data->ranges);
free_data:
	kfree(data);
	return ret;
}

static void anondax_remove(struct dev_dax *dev_dax)
{
	struct device *dev = &dev_dax->dev;
	struct anondax_data *data = dev_get_drvdata(dev);
	struct inode *inode = dax_inode(dev_dax->dax_dev);

	cdev_del(inode->i_cdev);
	kill_dev_dax(dev_dax);
	dev_set_drvdata(dev, NULL);

	if (offline_and_remove_memory_ranges(data->ranges, data->nr_ranges)) {
		dev_err(dev, "private memory stuck online until reboot\n");
		any_hotremove_failed = true;
	}

	kfree(data->ranges);
	kfree(data);
}

static struct dax_device_driver anondax_driver = {
	.probe = anondax_probe,
	.remove = anondax_remove,
	.type = DAXDRV_ANON_TYPE,
};

static int __init anondax_init(void)
{
	int ret;

	anondax_name = kstrdup_const("System RAM (anondax)", GFP_KERNEL);
	if (!anondax_name)
		return -ENOMEM;

	ret = dax_driver_register(&anondax_driver);
	if (ret)
		kfree_const(anondax_name);
	return ret;
}

static void __exit anondax_exit(void)
{
	dax_driver_unregister(&anondax_driver);
	if (!any_hotremove_failed)
		kfree_const(anondax_name);
}

module_init(anondax_init);
module_exit(anondax_exit);
MODULE_AUTHOR("Gregory Price <gourry@gourry.net>");
MODULE_DESCRIPTION("device-dax shared allocations from private memory");
MODULE_LICENSE("GPL");
