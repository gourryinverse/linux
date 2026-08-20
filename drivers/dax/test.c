// SPDX-License-Identifier: GPL-2.0
/*
 * Test provider for private memory nodes.
 *
 * Two independent halves, both test-only, so that neither has to live in
 * drivers/dax/kmem.c:
 *
 *   1. A dax provider.  It carves dax devices out of a range handed to it on
 *      the command line and declares their node, NODE_MEMORY_FEAT_* mask and
 *      abstract distance through the provider API, the way a real device
 *      driver for host-managed device memory would.  dax_kmem then derives
 *      private versus ordinary hotplug from the mask with no knob of its own.
 *      One device is created per declared mask, so a test can offer the same
 *      node two different masks and watch the second one be refused.
 *
 *   2. A node-bound anonymous mapping.  mmap() of <debugfs>/dax_test/anon
 *      returns ordinary anonymous memory bound to the node written to
 *      <debugfs>/dax_test/bind_node.  This is the only way to place memory on a
 *      node that did not opt into userspace placement, and unlike the dax
 *      device it works for nodes created with private_node= as well.
 */
#define pr_fmt(fmt) "dax_test: " fmt

#include <linux/debugfs.h>
#include <linux/memory.h>
#include <linux/memory-tiers.h>
#include <linux/memregion.h>
#include <linux/mempolicy.h>
#include <linux/module.h>
#include <linux/nodemask.h>
#include <linux/platform_device.h>
#include <linux/dax.h>
#include "bus.h"

/* Enough to hand one node several masks; nothing but a test needs >1. */
#define DAX_TEST_MAX_DEVS	4

static unsigned long range_start;
static unsigned long range_size;
static int target_node = NUMA_NO_NODE;
static unsigned long features[DAX_TEST_MAX_DEVS] = { NODE_MEMORY_FEAT_ALL };
static int nr_features = 1;
static int adistance;
static int adist_node = NUMA_NO_NODE;

module_param(range_start, ulong, 0444);
MODULE_PARM_DESC(range_start, "physical start of the range to carve (0 = none)");
module_param(range_size, ulong, 0444);
MODULE_PARM_DESC(range_size, "size of the range to carve");
module_param(target_node, int, 0444);
MODULE_PARM_DESC(target_node, "node the carved memory belongs to");
module_param_array(features, ulong, &nr_features, 0444);
MODULE_PARM_DESC(features, "NODE_MEMORY_FEAT_* mask, one dax device each");
module_param(adistance, int, 0644);
MODULE_PARM_DESC(adistance, "abstract distance to report for adist_node (0 = none)");
module_param(adist_node, int, 0644);
MODULE_PARM_DESC(adist_node, "node the declared abstract distance applies to");

static struct platform_device *dax_test_pdev;
static int dax_test_region_id = -1;
static struct dentry *dax_test_debugfs;

/* ------------------------------------------------------------------ */
/* 1. dax provider							*/
/* ------------------------------------------------------------------ */

static int dax_test_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dax_region *dax_region;
	resource_size_t size;
	struct range range = {
		.start = range_start,
		.end = range_start + range_size - 1,
	};
	int i;

	dax_region = alloc_dax_region(dev, pdev->id, &range, target_node,
				      PMD_SIZE, IORESOURCE_DAX_KMEM);
	if (!dax_region)
		return -ENOMEM;

	/* An even share per declared mask, each device its own memory block. */
	size = ALIGN_DOWN(range_len(&range) / nr_features,
			  memory_block_size_bytes());
	if (!size)
		return -EINVAL;

	for (i = 0; i < nr_features; i++) {
		struct dev_dax_data data = {
			.dax_region = dax_region,
			.id = -1,
			.size = size,
			.memmap_on_memory = false,
			/* what a real device driver would declare */
			.mm_features = features[i],
		};
		struct dev_dax *dev_dax = devm_create_dev_dax(&data);

		if (IS_ERR(dev_dax))
			return PTR_ERR(dev_dax);
	}

	return 0;
}

static struct platform_driver dax_test_driver = {
	.probe = dax_test_probe,
	.driver = {
		.name = "dax_test",
	},
};

/*
 * Abstract distance is answered through the memory-tier notifier chain rather
 * than a field on the device, which is how a real driver places its memory in
 * a tier -- and it works for a node that has no dax device at all.
 */
static int dax_test_adistance(struct notifier_block *nb, unsigned long nid,
			      void *data)
{
	int *adist = data;

	if (adist_node == NUMA_NO_NODE || (int)nid != adist_node || adistance <= 0)
		return NOTIFY_DONE;

	*adist = adistance;
	return NOTIFY_STOP;
}

static struct notifier_block dax_test_adistance_nb = {
	.notifier_call = dax_test_adistance,
};

/* ------------------------------------------------------------------ */
/* 2. node-bound anonymous mapping					*/
/* ------------------------------------------------------------------ */

static int bind_node = NUMA_NO_NODE;

static int dax_test_bind_node_get(void *priv, u64 *val)
{
	*val = bind_node;
	return 0;
}

static int dax_test_bind_node_set(void *priv, u64 val)
{
	if (val >= MAX_NUMNODES || !node_online(val))
		return -EINVAL;
	bind_node = val;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(dax_test_bind_node_fops, dax_test_bind_node_get,
			 dax_test_bind_node_set, "%llu\n");

/*
 * mmap() here yields a private anonymous mapping bound to @bind_node, which is
 * how a test gets memory onto a node it cannot name in a mempolicy.  Core mm
 * drops the file and treats the result as ordinary anonymous memory, so every
 * later operation on it takes the normal anon paths.
 */
static int dax_test_anon_mmap_prepare(struct vm_area_desc *desc)
{
	struct mempolicy *pol;
	nodemask_t nodes;

	if (bind_node == NUMA_NO_NODE || !node_online(bind_node))
		return -ENXIO;

	/* Private mappings are not shared by definition, reject MAP_SHARED. */
	if (vma_desc_test(desc, VMA_SHARED_BIT))
		return -EINVAL;

	/*
	 * A fresh policy per mapping, never a shared template: a VMA policy is
	 * mutated in place by cpuset rebind and by offline-time scrubbing, so a
	 * shared one would be corrupted for every later mapping.
	 */
	init_nodemask_of_node(&nodes, bind_node);
	pol = mempolicy_create(MPOL_BIND, MPOL_F_PRIVATE, &nodes);
	if (IS_ERR(pol))
		return PTR_ERR(pol);

	/*
	 * Anonymous in the sense core mm means it: no vm_ops, so every fault
	 * takes the ordinary anon path.  Same mechanism MAP_PRIVATE /dev/zero
	 * uses.  The mapping is bound by vm_policy.
	 */
	vma_desc_set_anonymous(desc);
	desc->vm_policy = pol;

	/*
	 * Default to base-page faults on a node without reclaim: without
	 * reclaim or compaction a high-order allocation can fail while memory
	 * is still available.
	 */
	if (!node_state(bind_node, N_MEMORY_RECLAIM))
		vma_desc_set_flags(desc, VMA_NOHUGEPAGE_BIT);

	return 0;
}

static const struct file_operations dax_test_anon_fops = {
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
	.mmap_prepare = dax_test_anon_mmap_prepare,
};

/* ------------------------------------------------------------------ */
/* 3. contiguous allocation						*/
/* ------------------------------------------------------------------ */

/*
 * Drives the contiguous allocator against the carved range so a test can
 * exercise both entry points on a private node:
 *
 *   echo "range <offset_pages> <nr_pages>" > contig   over the carved range
 *   echo "pfn <start_pfn> <nr_pages>" > contig       over any online PFNs
 *   echo "pages <nr_pages>" > contig                 searched, private node
 *   echo "public <nr_pages>" > contig                searched, plain
 *   echo "free" > contig
 *   cat contig     ->  "pfn=<pfn> nr=<n> nid=<nid> rc=<rc>"
 *
 * "pages" and "public" differ only in whether the search may see a private
 * node, which is the distinction the search variant exists to make: "public"
 * naming a private target is expected to land somewhere else entirely.
 *
 * One outstanding allocation at a time; that is all a test needs.
 */
static DEFINE_MUTEX(dax_test_contig_lock);
static unsigned long dax_test_contig_pfn;
static unsigned long dax_test_contig_nr;
static int dax_test_contig_rc;

static void dax_test_contig_release(void)
{
	if (!dax_test_contig_nr)
		return;
	free_contig_range(dax_test_contig_pfn, dax_test_contig_nr);
	dax_test_contig_pfn = 0;
	dax_test_contig_nr = 0;
}

static int dax_test_contig_alloc_pfn(unsigned long start, unsigned long nr)
{
	int rc;

	if (!nr)
		return -EINVAL;
	if (!pfn_to_online_page(start) || !pfn_to_online_page(start + nr - 1))
		return -ENXIO;

	rc = alloc_contig_range(start, start + nr, ACR_FLAGS_NONE, GFP_KERNEL);
	if (rc)
		return rc;

	dax_test_contig_pfn = start;
	dax_test_contig_nr = nr;
	return 0;
}

static int dax_test_contig_alloc_range(unsigned long off, unsigned long nr)
{
	if (!range_size)
		return -EINVAL;
	if (off + nr > PHYS_PFN(range_size))
		return -ERANGE;

	return dax_test_contig_alloc_pfn(PHYS_PFN(range_start) + off, nr);
}

/* Searched allocation; @private decides whether it may see a private node. */
static int dax_test_contig_alloc_pages(unsigned long nr, bool private)
{
	struct page *page;

	if (!nr || target_node == NUMA_NO_NODE)
		return -EINVAL;

	/*
	 * Movable-capable: dax/kmem onlines into ZONE_MOVABLE by default, and
	 * a GFP_KERNEL search stops at ZONE_NORMAL and never sees the node.
	 */
	if (private)
		page = alloc_contig_pages_private(nr, GFP_HIGHUSER_MOVABLE,
						  target_node);
	else
		page = alloc_contig_pages(nr, GFP_HIGHUSER_MOVABLE,
					  target_node, NULL);
	if (!page)
		return -ENOMEM;

	dax_test_contig_pfn = page_to_pfn(page);
	dax_test_contig_nr = nr;
	return 0;
}

static ssize_t dax_test_contig_write(struct file *file, const char __user *buf,
				     size_t len, loff_t *ppos)
{
	unsigned long a, b;
	char cmd[64];
	ssize_t rc;

	if (len >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, buf, len))
		return -EFAULT;
	cmd[len] = '\0';

	guard(mutex)(&dax_test_contig_lock);

	if (sysfs_streq(cmd, "free")) {
		dax_test_contig_release();
		dax_test_contig_rc = 0;
		return len;
	}

	/* A second allocation would leak the first. */
	if (dax_test_contig_nr)
		return -EBUSY;

	if (sscanf(cmd, "range %lu %lu", &a, &b) == 2)
		rc = dax_test_contig_alloc_range(a, b);
	else if (sscanf(cmd, "pfn %lu %lu", &a, &b) == 2)
		rc = dax_test_contig_alloc_pfn(a, b);
	else if (sscanf(cmd, "pages %lu", &a) == 1)
		rc = dax_test_contig_alloc_pages(a, true);
	else if (sscanf(cmd, "public %lu", &a) == 1)
		rc = dax_test_contig_alloc_pages(a, false);
	else
		return -EINVAL;

	dax_test_contig_rc = rc;

	/*
	 * A refused allocation is a result the test wants to read back, not a
	 * write error: report it through the file, not through errno.
	 */
	return len;
}

static int dax_test_contig_show(struct seq_file *m, void *v)
{
	guard(mutex)(&dax_test_contig_lock);

	seq_printf(m, "pfn=%lu nr=%lu nid=%d rc=%d\n",
		   dax_test_contig_pfn, dax_test_contig_nr,
		   dax_test_contig_nr ?
			   page_to_nid(pfn_to_page(dax_test_contig_pfn)) : -1,
		   dax_test_contig_rc);
	return 0;
}

static int dax_test_contig_open(struct inode *inode, struct file *file)
{
	return single_open(file, dax_test_contig_show, NULL);
}

static const struct file_operations dax_test_contig_fops = {
	.owner = THIS_MODULE,
	.open = dax_test_contig_open,
	.read = seq_read,
	.write = dax_test_contig_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* ------------------------------------------------------------------ */

static int __init dax_test_init(void)
{
	int rc;

	dax_test_debugfs = debugfs_create_dir("dax_test", NULL);
	debugfs_create_file_unsafe("bind_node", 0600, dax_test_debugfs, NULL,
				   &dax_test_bind_node_fops);
	/*
	 * _unsafe: debugfs's full proxy forwards only read/write/llseek/ioctl,
	 * so a proxied file cannot be mmap()ed at all.  Safe here because the
	 * mapping is anonymised, leaving no reference to these fops once
	 * mmap() has returned.
	 */
	debugfs_create_file_unsafe("anon", 0600, dax_test_debugfs, NULL,
				   &dax_test_anon_fops);
	debugfs_create_file("contig", 0600, dax_test_debugfs, NULL,
			    &dax_test_contig_fops);

	rc = register_mt_adistance_algorithm(&dax_test_adistance_nb);
	if (rc)
		goto err_debugfs;

	rc = platform_driver_register(&dax_test_driver);
	if (rc)
		goto err_notifier;

	/* A range is optional: the anon mapping alone needs no dax device. */
	if (!range_size)
		return 0;

	/*
	 * The platform device id becomes the dax region id, so take one from
	 * the global pool rather than assuming 0 is free -- another provider
	 * has usually claimed it by now.
	 */
	rc = memregion_alloc(GFP_KERNEL);
	if (rc < 0)
		goto err_driver;
	dax_test_region_id = rc;

	dax_test_pdev = platform_device_register_simple("dax_test",
							dax_test_region_id,
							NULL, 0);
	if (IS_ERR(dax_test_pdev)) {
		rc = PTR_ERR(dax_test_pdev);
		goto err_memregion;
	}
	return 0;

err_memregion:
	memregion_free(dax_test_region_id);
	dax_test_region_id = -1;

err_driver:
	platform_driver_unregister(&dax_test_driver);
err_notifier:
	unregister_mt_adistance_algorithm(&dax_test_adistance_nb);
err_debugfs:
	debugfs_remove_recursive(dax_test_debugfs);
	return rc;
}

static void __exit dax_test_exit(void)
{
	/* An outstanding allocation would keep the memory from being removed. */
	scoped_guard(mutex, &dax_test_contig_lock)
		dax_test_contig_release();

	if (!IS_ERR_OR_NULL(dax_test_pdev))
		platform_device_unregister(dax_test_pdev);
	if (dax_test_region_id >= 0)
		memregion_free(dax_test_region_id);
	platform_driver_unregister(&dax_test_driver);
	unregister_mt_adistance_algorithm(&dax_test_adistance_nb);
	debugfs_remove_recursive(dax_test_debugfs);
}

module_init(dax_test_init);
module_exit(dax_test_exit);
MODULE_DESCRIPTION("Test provider for private memory nodes");
MODULE_LICENSE("GPL");
