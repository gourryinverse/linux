// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/misc/cma_private_test.c - Named CMA pool test driver
 *
 * Simple test module that claims a cma_private= pool and exposes
 * basic CMA alloc/free via debugfs for testing.
 */

#include <linux/cma.h>
#include <linux/debugfs.h>
#include <linux/module.h>

static char *name = "";
module_param(name, charp, 0444);
MODULE_PARM_DESC(name, "CMA private pool name to claim");

static struct cma *cma;
static struct dentry *debugfs_dir;

/* debugfs: write number of pages to allocate, read result */
static struct page *test_pages;
static unsigned long test_count;

static ssize_t alloc_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	unsigned long nr_pages;
	int ret;

	ret = kstrtoul_from_user(buf, count, 0, &nr_pages);
	if (ret)
		return ret;

	/* changed */
	if (test_pages) {
		pr_err("cma_private_test: pages already allocated, free first\n");
		return -EBUSY;
	}

	if (!nr_pages)
		return -EINVAL;

	test_pages = cma_alloc(cma, nr_pages, 0, false);
	if (!test_pages) {
		pr_err("cma_private_test: alloc %lu pages failed\n", nr_pages);
		return -ENOMEM;
	}

	test_count = nr_pages;
	pr_info("cma_private_test: allocated %lu pages at pfn %lx\n",
		nr_pages, page_to_pfn(test_pages));

	return count;
}

static const struct file_operations alloc_fops = {
	.write = alloc_write,
};

static ssize_t free_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(buf, count, 0, &val);
	if (ret)
		return ret;

	if (!test_pages) {
		pr_err("cma_private_test: no pages allocated\n");
		return -ENOENT;
	}

	if (!cma_release(cma, test_pages, test_count)) {
		pr_err("cma_private_test: release failed\n");
		return -EINVAL;
	}

	pr_info("cma_private_test: freed %lu pages\n", test_count);
	test_pages = NULL;
	test_count = 0;

	return count;
}

static const struct file_operations free_fops = {
	.write = free_write,
};

static int __init cma_private_test_init(void)
{
	int rc;

	if (!name || !name[0]) {
		pr_err("cma_private_test: 'name' parameter required\n");
		return -EINVAL;
	}

	rc = cma_private_claim(name, &cma);
	if (rc) {
		pr_err("cma_private_test: claim '%s' failed: %d\n", name, rc);
		return rc;
	}

	pr_info("cma_private_test: claimed '%s', base %pa size %lu pages\n",
		name, &(phys_addr_t){cma_get_base(cma)},
		cma_get_size(cma) >> PAGE_SHIFT);

	debugfs_dir = debugfs_create_dir("cma_private_test", NULL);
	debugfs_create_file("alloc", 0200, debugfs_dir, NULL, &alloc_fops);
	debugfs_create_file("free", 0200, debugfs_dir, NULL, &free_fops);

	return 0;
}

static void __exit cma_private_test_exit(void)
{
	debugfs_remove_recursive(debugfs_dir);

	if (test_pages) {
		cma_release(cma, test_pages, test_count);
		test_pages = NULL;
		test_count = 0;
	}

	cma_private_release(cma);
	pr_info("cma_private_test: released '%s'\n", name);
}

module_init(cma_private_test_init);
module_exit(cma_private_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Named CMA pool test driver");
