// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Meta Platforms, Inc. All rights reserved. */
/*
 * CXL Compression Driver
 *
 * This driver provides an alternative binding for CXL memory devices that
 * converts all associated RAM regions to sysram_regions for direct memory
 * hotplug, bypassing the standard dax region path.
 *
 * Page reclamation uses the standard CXL Media Operations Zero command
 * (opcode 0x4402, class 0x01, subclass 0x01).  Watermark interrupts
 * are delivered via separate MSI-X vectors (12 for lthresh, 13 for
 * hthresh), injected externally via QMP.
 *
 * Usage:
 *   1. Device initially binds to cxl_pci at boot
 *   2. Unbind from cxl_pci:
 *        echo $PCI_DEV > /sys/bus/pci/drivers/cxl_pci/unbind
 *   3. Bind to cxl_compression:
 *        echo $PCI_DEV > /sys/bus/pci/drivers/cxl_compression/bind
 */

#include <linux/unaligned.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/sizes.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/bitmap.h>
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/cram.h>
#include <linux/memory_hotplug.h>
#include <linux/xarray.h>
#include <cxl/mailbox.h>
#include "cxlmem.h"
#include "cxl.h"

/*
 * Per-device compression context lookup.
 *
 * pci_set_drvdata() MUST store cxlds because mbox_to_cxlds() uses
 * dev_get_drvdata() to recover the cxl_dev_state from the mailbox host
 * device.  Storing anything else in pci drvdata breaks every CXL mailbox
 * command.  Use an xarray keyed by pci_dev pointer so that multiple
 * devices can bind concurrently without colliding.
 */
static DEFINE_XARRAY(comp_ctx_xa);

static struct cxl_compression_ctx *pdev_to_comp_ctx(struct pci_dev *pdev)
{
	return xa_load(&comp_ctx_xa, (unsigned long)pdev);
}

#define CXL_MEDIA_OP_OPCODE		0x4402
#define CXL_MEDIA_OP_CLASS_SANITIZE	0x01
#define CXL_MEDIA_OP_SUBC_ZERO		0x01

struct cxl_dpa_range {
	__le64 starting_dpa;
	__le64 length;
} __packed;

struct cxl_media_op_input {
	u8 media_operation_class;
	u8 media_operation_subclass;
	__le16 reserved;
	__le32 dpa_range_count;
	struct cxl_dpa_range ranges[];
} __packed;

#define CXL_CT3_MSIX_LTHRESH		12
#define CXL_CT3_MSIX_HTHRESH		13
#define CXL_CT3_MSIX_VECTOR_NR		14
#define CXL_FLUSH_INTERVAL_DEFAULT_MS	1000

static unsigned int flush_buf_size;
module_param(flush_buf_size, uint, 0444);
MODULE_PARM_DESC(flush_buf_size,
		 "Max DPA ranges per media ops CCI command (0 = use hw max)");

static unsigned int flush_interval_ms = CXL_FLUSH_INTERVAL_DEFAULT_MS;
module_param(flush_interval_ms, uint, 0644);
MODULE_PARM_DESC(flush_interval_ms,
		 "Flush worker interval in ms (default 1000)");

struct cxl_flush_buf {
	unsigned int count;
	unsigned int max;			/* max ranges per command */
	struct cxl_media_op_input *cmd;		/* pre-allocated CCI payload */
	struct folio **folios;			/* parallel folio tracking */
};

struct cxl_flush_ctx;

struct cxl_pcpu_flush {
	struct cxl_flush_buf __rcu *active;	/* callback writes here */
	struct cxl_flush_buf *overflow_spare;	/* spare for overflow work */
	struct work_struct overflow_work;	/* per-CPU overflow flush */
	struct cxl_flush_ctx *ctx;		/* backpointer */
};

/**
 * struct cxl_flush_ctx - Per-region flush context
 * @flush_record: two-level bitmap, 1 bit per 4KB page, tracks in-flight ops
 * @flush_record_pages: number of pages in the flush_record array
 * @nr_pages: total number of 4KB pages in the region
 * @base_pfn: starting PFN of the region (for DPA offset calculation)
 * @buf_max: max DPA ranges per CCI command
 * @media_ops_supported: true if device supports media operations zero
 * @pcpu: per-CPU flush state
 * @kthread_spares: array[nr_cpu_ids] of spare buffers for the kthread
 * @flush_thread: round-robin kthread
 * @mbox: pointer to CXL mailbox for sending CCI commands
 * @dev: device for logging
 * @nid: NUMA node of the private region
 */
struct cxl_flush_ctx {
	unsigned long	**flush_record;
	unsigned int	 flush_record_pages;
	unsigned long	 nr_pages;
	unsigned long	 base_pfn;
	unsigned int	 buf_max;
	bool		 media_ops_supported;
	struct cxl_pcpu_flush __percpu *pcpu;
	struct cxl_flush_buf **kthread_spares;
	struct task_struct *flush_thread;
	struct cxl_mailbox *mbox;
	struct device	*dev;
	int		 nid;
};

/* Bits per page-sized bitmap chunk */
#define FLUSH_RECORD_BITS_PER_PAGE	(PAGE_SIZE * BITS_PER_BYTE)
#define FLUSH_RECORD_SHIFT		(PAGE_SHIFT + 3)

static unsigned long **flush_record_alloc(unsigned long nr_bits,
					  unsigned int *nr_pages_out)
{
	unsigned int nr_pages = DIV_ROUND_UP(nr_bits, FLUSH_RECORD_BITS_PER_PAGE);
	unsigned long **pages;
	unsigned int i;

	pages = kcalloc(nr_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;

	for (i = 0; i < nr_pages; i++) {
		pages[i] = (unsigned long *)get_zeroed_page(GFP_KERNEL);
		if (!pages[i])
			goto err;
	}

	*nr_pages_out = nr_pages;
	return pages;

err:
	while (i--)
		free_page((unsigned long)pages[i]);
	kfree(pages);
	return NULL;
}

static void flush_record_free(unsigned long **pages, unsigned int nr_pages)
{
	unsigned int i;

	if (!pages)
		return;

	for (i = 0; i < nr_pages; i++)
		free_page((unsigned long)pages[i]);
	kfree(pages);
}

static inline bool flush_record_test_and_clear(unsigned long **pages,
					       unsigned long idx)
{
	return test_and_clear_bit(idx & (FLUSH_RECORD_BITS_PER_PAGE - 1),
				  pages[idx >> FLUSH_RECORD_SHIFT]);
}

static inline void flush_record_set(unsigned long **pages, unsigned long idx)
{
	set_bit(idx & (FLUSH_RECORD_BITS_PER_PAGE - 1),
		pages[idx >> FLUSH_RECORD_SHIFT]);
}

static struct cxl_flush_buf *cxl_flush_buf_alloc(unsigned int max, int nid)
{
	struct cxl_flush_buf *buf;

	buf = kzalloc_node(sizeof(*buf), GFP_KERNEL, nid);
	if (!buf)
		return NULL;

	buf->max = max;
	buf->cmd = kvzalloc_node(struct_size(buf->cmd, ranges, max),
				 GFP_KERNEL, nid);
	if (!buf->cmd)
		goto err_cmd;

	buf->folios = kcalloc_node(max, sizeof(struct folio *),
				   GFP_KERNEL, nid);
	if (!buf->folios)
		goto err_folios;

	return buf;

err_folios:
	kvfree(buf->cmd);
err_cmd:
	kfree(buf);
	return NULL;
}

static void cxl_flush_buf_free(struct cxl_flush_buf *buf)
{
	if (!buf)
		return;
	kvfree(buf->cmd);
	kfree(buf->folios);
	kfree(buf);
}

static inline void cxl_flush_buf_reset(struct cxl_flush_buf *buf)
{
	buf->count = 0;
}

static void cxl_flush_buf_send(struct cxl_flush_ctx *ctx,
			       struct cxl_flush_buf *buf)
{
	struct cxl_mbox_cmd mbox_cmd;
	unsigned int count = buf->count;
	unsigned int i;
	int rc;

	if (count == 0)
		return;

	if (!ctx->media_ops_supported) {
		/* No device support, zero all folios inline */
		for (i = 0; i < count; i++)
			folio_zero_range(buf->folios[i], 0,
					 folio_size(buf->folios[i]));
		goto release;
	}

	buf->cmd->media_operation_class = CXL_MEDIA_OP_CLASS_SANITIZE;
	buf->cmd->media_operation_subclass = CXL_MEDIA_OP_SUBC_ZERO;
	buf->cmd->reserved = 0;
	buf->cmd->dpa_range_count = cpu_to_le32(count);

	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MEDIA_OP_OPCODE,
		.payload_in = buf->cmd,
		.size_in = struct_size(buf->cmd, ranges, count),
		.poll_interval_ms = 1000,
		.poll_count = 30,
	};

	rc = cxl_internal_send_cmd(ctx->mbox, &mbox_cmd);
	if (rc) {
		dev_warn(ctx->dev,
			 "media ops zero CCI command failed: %d\n", rc);

		/* Zero all folios inline on failure */
		for (i = 0; i < count; i++)
			folio_zero_range(buf->folios[i], 0,
					 folio_size(buf->folios[i]));
	}

release:
	for (i = 0; i < count; i++)
		folio_put(buf->folios[i]);

	cxl_flush_buf_reset(buf);
}

static int cxl_compression_flush_cb(struct folio *folio, void *private)
{
	struct cxl_flush_ctx *ctx = private;
	unsigned long pfn = folio_pfn(folio);
	unsigned long idx = pfn - ctx->base_pfn;
	unsigned long nr = folio_nr_pages(folio);
	struct cxl_pcpu_flush *pcpu;
	struct cxl_flush_buf *buf;
	unsigned long flags;
	unsigned int pos;

	/* Case (a): flush record bit set, resolution from our media op */
	if (flush_record_test_and_clear(ctx->flush_record, idx))
		return 0;

	dev_dbg_ratelimited(ctx->dev,
			     "flush_cb: folio pfn=%lx order=%u idx=%lu cpu=%d\n",
			     pfn, folio_order(folio), idx,
			     raw_smp_processor_id());

	local_irq_save(flags);
	rcu_read_lock();

	pcpu = this_cpu_ptr(ctx->pcpu);
	buf = rcu_dereference(pcpu->active);

	if (unlikely(!buf || buf->count >= buf->max)) {
		rcu_read_unlock();
		local_irq_restore(flags);
		if (buf)
			schedule_work_on(raw_smp_processor_id(),
					 &pcpu->overflow_work);
		return 2;
	}

	/* Case (b): write DPA range directly into pre-formatted CCI buffer */
	folio_get(folio);
	flush_record_set(ctx->flush_record, idx);

	pos = buf->count;
	buf->folios[pos] = folio;
	buf->cmd->ranges[pos].starting_dpa = cpu_to_le64((u64)idx * PAGE_SIZE);
	buf->cmd->ranges[pos].length = cpu_to_le64((u64)nr * PAGE_SIZE);
	buf->count = pos + 1;

	rcu_read_unlock();
	local_irq_restore(flags);

	return 1;
}

static int cxl_flush_kthread_fn(void *data)
{
	struct cxl_flush_ctx *ctx = data;
	struct cxl_flush_buf *dirty;
	struct cxl_pcpu_flush *pcpu;
	int cpu;
	bool any_dirty;

	while (!kthread_should_stop()) {
		any_dirty = false;

		/* Phase 1: Swap all per-CPU buffers */
		for_each_possible_cpu(cpu) {
			struct cxl_flush_buf *spare = ctx->kthread_spares[cpu];

			if (!spare)
				continue;

			pcpu = per_cpu_ptr(ctx->pcpu, cpu);
			cxl_flush_buf_reset(spare);
			dirty = rcu_replace_pointer(pcpu->active, spare, true);
			ctx->kthread_spares[cpu] = dirty;

			if (dirty && dirty->count > 0) {
				dev_dbg(ctx->dev,
					 "flush_kthread: cpu=%d has %u dirty ranges\n",
					 cpu, dirty->count);
				any_dirty = true;
			}
		}

		if (!any_dirty)
			goto sleep;

		/* Phase 2: Single synchronize_rcu for all swaps */
		synchronize_rcu();

		/* Phase 3: Send CCI commands for dirty buffers */
		for_each_possible_cpu(cpu) {
			dirty = ctx->kthread_spares[cpu];
			if (dirty && dirty->count > 0)
				cxl_flush_buf_send(ctx, dirty);
			/* dirty is now clean, stays as kthread_spares[cpu] */
		}

sleep:
		schedule_timeout_interruptible(
			msecs_to_jiffies(flush_interval_ms));
	}

	return 0;
}

static void cxl_flush_overflow_work(struct work_struct *work)
{
	struct cxl_pcpu_flush *pcpu =
		container_of(work, struct cxl_pcpu_flush, overflow_work);
	struct cxl_flush_ctx *ctx = pcpu->ctx;
	struct cxl_flush_buf *dirty, *spare;
	unsigned long flags;

	dev_dbg(ctx->dev, "flush_overflow: cpu=%d buffer full, flushing\n",
		 raw_smp_processor_id());

	spare = pcpu->overflow_spare;
	if (!spare)
		return;

	cxl_flush_buf_reset(spare);

	local_irq_save(flags);
	dirty = rcu_replace_pointer(pcpu->active, spare, true);
	local_irq_restore(flags);

	pcpu->overflow_spare = dirty;

	synchronize_rcu();
	cxl_flush_buf_send(ctx, dirty);
}

struct cxl_teardown_ctx {
	struct cxl_flush_ctx *flush_ctx;
	struct cxl_sysram *sysram;
	int nid;
};

static void cxl_compression_pre_teardown(void *data)
{
	struct cxl_teardown_ctx *tctx = data;

	if (!tctx->flush_ctx)
		return;

	/*
	 * Unregister the CRAM node before memory goes offline.
	 * node_private_clear_ops requires the node_private to still
	 * exist, which is destroyed during memory removal.
	 */
	cram_unregister_private_node(tctx->nid);

	/*
	 * Offline and remove CXL memory with retry.  CXL compressed
	 * memory may have pages pinned by in-flight flush operations;
	 * keep retrying until they complete.  Once done, sysram->res
	 * is NULL so the devm sysram_unregister action that follows
	 * will skip the hotplug removal.
	 */
	if (tctx->sysram) {
		int rc, retries = 0;

		while (true) {
			rc = cxl_sysram_offline_and_remove(tctx->sysram);
			if (!rc)
				break;
			if (++retries > 60) {
				pr_err("cxl_compression: memory offline failed after %d retries, giving up\n",
				       retries);
				break;
			}
			pr_info("cxl_compression: memory offline failed (%d), retrying...\n",
				rc);
			msleep(1000);
		}
	}
}

static void cxl_compression_post_teardown(void *data)
{
	struct cxl_teardown_ctx *tctx = data;
	struct cxl_flush_ctx *ctx = tctx->flush_ctx;
	struct cxl_pcpu_flush *pcpu;
	struct cxl_flush_buf *buf;
	int cpu;

	if (!ctx)
		return;

	/* cram_unregister_private_node already called in pre_teardown */

	if (ctx->flush_thread) {
		kthread_stop(ctx->flush_thread);
		ctx->flush_thread = NULL;
	}

	for_each_possible_cpu(cpu) {
		pcpu = per_cpu_ptr(ctx->pcpu, cpu);
		cancel_work_sync(&pcpu->overflow_work);
	}

	for_each_possible_cpu(cpu) {
		pcpu = per_cpu_ptr(ctx->pcpu, cpu);

		buf = rcu_dereference_raw(pcpu->active);
		if (buf && buf->count > 0)
			cxl_flush_buf_send(ctx, buf);

		if (pcpu->overflow_spare && pcpu->overflow_spare->count > 0)
			cxl_flush_buf_send(ctx, pcpu->overflow_spare);

		if (ctx->kthread_spares && ctx->kthread_spares[cpu]) {
			buf = ctx->kthread_spares[cpu];
			if (buf->count > 0)
				cxl_flush_buf_send(ctx, buf);
		}
	}

	for_each_possible_cpu(cpu) {
		pcpu = per_cpu_ptr(ctx->pcpu, cpu);

		buf = rcu_dereference_raw(pcpu->active);
		cxl_flush_buf_free(buf);

		cxl_flush_buf_free(pcpu->overflow_spare);

		if (ctx->kthread_spares)
			cxl_flush_buf_free(ctx->kthread_spares[cpu]);
	}

	kfree(ctx->kthread_spares);
	free_percpu(ctx->pcpu);
	flush_record_free(ctx->flush_record, ctx->flush_record_pages);
}

/**
 * struct cxl_compression_ctx - Per-device context for compression driver
 * @mbox: CXL mailbox for issuing CCI commands
 * @pdev: PCI device
 * @flush_ctx: Flush context for deferred page reclamation
 * @tctx: Teardown context for devm actions
 * @sysram: Sysram device for offline+remove in remove path
 * @nid: NUMA node ID, NUMA_NO_NODE if unset
 * @cxlmd: The memdev associated with this context
 * @cxlr: Region created by this driver (NULL if pre-existing)
 * @cxled: Endpoint decoder with DPA allocated by this driver
 * @regions_converted: Number of regions successfully converted
 * @media_ops_supported: Device supports media operations zero (0x4402)
 */
struct cxl_compression_ctx {
	struct cxl_mailbox *mbox;
	struct pci_dev *pdev;
	struct cxl_flush_ctx *flush_ctx;
	struct cxl_teardown_ctx *tctx;
	struct cxl_sysram *sysram;
	int nid;
	struct cxl_memdev *cxlmd;
	struct cxl_region *cxlr;
	struct cxl_endpoint_decoder *cxled;
	int regions_converted;
	bool media_ops_supported;
};

/*
 * Probe whether the device supports Media Operations Zero (0x4402).
 * Send a zero-count command, a conforming device returns SUCCESS,
 * a device that doesn't support it returns UNSUPPORTED (-ENXIO).
 */
static bool cxl_probe_media_ops_zero(struct cxl_mailbox *mbox,
				     struct device *dev)
{
	struct cxl_media_op_input probe = {
		.media_operation_class = CXL_MEDIA_OP_CLASS_SANITIZE,
		.media_operation_subclass = CXL_MEDIA_OP_SUBC_ZERO,
		.dpa_range_count = 0,
	};
	struct cxl_mbox_cmd cmd = {
		.opcode = CXL_MEDIA_OP_OPCODE,
		.payload_in = &probe,
		.size_in = sizeof(probe),
	};
	int rc;

	rc = cxl_internal_send_cmd(mbox, &cmd);
	if (rc) {
		dev_info(dev,
			 "media operations zero not supported (rc=%d), using inline zeroing\n",
			 rc);
		return false;
	}

	dev_info(dev, "media operations zero (0x4402) supported\n");
	return true;
}

struct cxl_compression_wm_ctx {
	struct device *dev;
	int nid;
};

static irqreturn_t cxl_compression_lthresh_irq(int irq, void *data)
{
	struct cxl_compression_wm_ctx *wm = data;

	dev_info(wm->dev, "lthresh watermark: pressuring node %d\n", wm->nid);
	cram_set_pressure(wm->nid, CRAM_PRESSURE_MAX);
	return IRQ_HANDLED;
}

static irqreturn_t cxl_compression_hthresh_irq(int irq, void *data)
{
	struct cxl_compression_wm_ctx *wm = data;

	dev_info(wm->dev, "hthresh watermark: resuming node %d\n", wm->nid);
	cram_set_pressure(wm->nid, 0);
	return IRQ_HANDLED;
}

static int convert_dvsec_to_sysram(struct range *hpa, struct pci_dev *pdev)
{
	struct cxl_compression_ctx *comp_ctx = pdev_to_comp_ctx(pdev);
	struct device *dev = &pdev->dev;
	struct cxl_compression_wm_ctx *wm_ctx;
	struct cxl_teardown_ctx *tctx;
	struct cxl_flush_ctx *flush_ctx;
	struct cxl_pcpu_flush *pcpu;
	struct cxl_sysram *sysram;
	struct device *sdev;
	resource_size_t region_start, region_size;
	char sname[64];
	int nid;
	int irq;
	int cpu;
	int rc;

	dev_info(dev, "DVSEC fallback: converting range [%#llx-%#llx] to sysram\n",
		 (u64)hpa->start, (u64)hpa->end);

	rc = devm_cxl_add_sysram_range(dev, hpa, true, MMOP_ONLINE_MOVABLE);
	if (rc) {
		dev_err(dev, "failed to add sysram range: %d\n", rc);
		return rc;
	}

	/* Find the sysram child device we just created */
	snprintf(sname, sizeof(sname), "sysram_%s", dev_name(dev));
	sdev = device_find_child_by_name(dev, sname);
	if (!sdev) {
		dev_err(dev, "failed to find sysram device %s\n", sname);
		return -ENODEV;
	}
	sysram = to_cxl_sysram(sdev);
	comp_ctx->sysram = sysram;

	tctx = devm_kzalloc(dev, sizeof(*tctx), GFP_KERNEL);
	if (!tctx) {
		put_device(sdev);
		return -ENOMEM;
	}

	rc = devm_add_action_or_reset(dev, cxl_compression_post_teardown, tctx);
	if (rc) {
		put_device(sdev);
		return rc;
	}

	tctx->sysram = sysram;

	nid = sysram->numa_node;
	region_start = hpa->start;
	region_size = range_len(hpa);

	flush_ctx = devm_kzalloc(dev, sizeof(*flush_ctx), GFP_KERNEL);
	if (!flush_ctx) {
		put_device(sdev);
		return -ENOMEM;
	}

	flush_ctx->base_pfn = PHYS_PFN(region_start);
	flush_ctx->nr_pages = region_size >> PAGE_SHIFT;
	flush_ctx->flush_record = flush_record_alloc(flush_ctx->nr_pages,
						     &flush_ctx->flush_record_pages);
	if (!flush_ctx->flush_record) {
		put_device(sdev);
		return -ENOMEM;
	}

	flush_ctx->mbox = comp_ctx->mbox;
	flush_ctx->dev = dev;
	flush_ctx->nid = nid;
	flush_ctx->media_ops_supported = comp_ctx->media_ops_supported;

	flush_ctx->buf_max = (flush_ctx->mbox->payload_size -
			      sizeof(struct cxl_media_op_input)) /
			     sizeof(struct cxl_dpa_range);
	if (flush_buf_size && flush_buf_size < flush_ctx->buf_max)
		flush_ctx->buf_max = flush_buf_size;
	if (flush_ctx->buf_max == 0)
		flush_ctx->buf_max = 1;

	dev_info(dev,
		 "flush buffer: %u DPA ranges per command (payload %zu bytes, media_ops %s)\n",
		 flush_ctx->buf_max, flush_ctx->mbox->payload_size,
		 flush_ctx->media_ops_supported ? "yes" : "no");

	flush_ctx->pcpu = alloc_percpu(struct cxl_pcpu_flush);
	if (!flush_ctx->pcpu) {
		put_device(sdev);
		return -ENOMEM;
	}

	flush_ctx->kthread_spares = kcalloc(nr_cpu_ids,
					    sizeof(struct cxl_flush_buf *),
					    GFP_KERNEL);
	if (!flush_ctx->kthread_spares)
		goto err_pcpu_init;

	for_each_possible_cpu(cpu) {
		struct cxl_flush_buf *active_buf, *overflow_buf, *spare_buf;

		active_buf = cxl_flush_buf_alloc(flush_ctx->buf_max, nid);
		if (!active_buf)
			goto err_pcpu_init;

		overflow_buf = cxl_flush_buf_alloc(flush_ctx->buf_max, nid);
		if (!overflow_buf) {
			cxl_flush_buf_free(active_buf);
			goto err_pcpu_init;
		}

		spare_buf = cxl_flush_buf_alloc(flush_ctx->buf_max, nid);
		if (!spare_buf) {
			cxl_flush_buf_free(active_buf);
			cxl_flush_buf_free(overflow_buf);
			goto err_pcpu_init;
		}

		pcpu = per_cpu_ptr(flush_ctx->pcpu, cpu);
		pcpu->ctx = flush_ctx;
		rcu_assign_pointer(pcpu->active, active_buf);
		pcpu->overflow_spare = overflow_buf;
		INIT_WORK(&pcpu->overflow_work, cxl_flush_overflow_work);

		flush_ctx->kthread_spares[cpu] = spare_buf;
	}

	flush_ctx->flush_thread = kthread_create_on_node(
		cxl_flush_kthread_fn, flush_ctx, nid, "cxl-flush/%d", nid);
	if (IS_ERR(flush_ctx->flush_thread)) {
		rc = PTR_ERR(flush_ctx->flush_thread);
		flush_ctx->flush_thread = NULL;
		goto err_pcpu_init;
	}
	wake_up_process(flush_ctx->flush_thread);

	/* Use sysram device pointer as owner key (no region) */
	rc = cram_register_private_node(nid, sysram,
					cxl_compression_flush_cb, flush_ctx);
	if (rc) {
		dev_err(dev, "failed to register cram node %d: %d\n", nid, rc);
		goto err_pcpu_init;
	}

	tctx->flush_ctx = flush_ctx;
	tctx->nid = nid;

	rc = devm_add_action_or_reset(dev, cxl_compression_pre_teardown, tctx);
	if (rc) {
		put_device(sdev);
		return rc;
	}

	comp_ctx->flush_ctx = flush_ctx;
	comp_ctx->tctx = tctx;
	comp_ctx->nid = nid;

	/* Register watermark IRQ handlers */
	wm_ctx = devm_kzalloc(dev, sizeof(*wm_ctx), GFP_KERNEL);
	if (!wm_ctx) {
		put_device(sdev);
		return -ENOMEM;
	}

	wm_ctx->dev = dev;
	wm_ctx->nid = nid;

	irq = pci_irq_vector(pdev, CXL_CT3_MSIX_LTHRESH);
	if (irq >= 0) {
		rc = devm_request_threaded_irq(dev, irq, NULL,
					       cxl_compression_lthresh_irq,
					       IRQF_ONESHOT,
					       "cxl-lthresh", wm_ctx);
		if (rc)
			dev_warn(dev, "failed to register lthresh IRQ: %d\n",
				 rc);
	}

	irq = pci_irq_vector(pdev, CXL_CT3_MSIX_HTHRESH);
	if (irq >= 0) {
		rc = devm_request_threaded_irq(dev, irq, NULL,
					       cxl_compression_hthresh_irq,
					       IRQF_ONESHOT,
					       "cxl-hthresh", wm_ctx);
		if (rc)
			dev_warn(dev, "failed to register hthresh IRQ: %d\n",
				 rc);
	}

	put_device(sdev);
	return 0;

err_pcpu_init:
	put_device(sdev);
	if (flush_ctx->flush_thread)
		kthread_stop(flush_ctx->flush_thread);
	for_each_possible_cpu(cpu) {
		struct cxl_flush_buf *buf;

		pcpu = per_cpu_ptr(flush_ctx->pcpu, cpu);

		buf = rcu_dereference_raw(pcpu->active);
		cxl_flush_buf_free(buf);

		cxl_flush_buf_free(pcpu->overflow_spare);

		if (flush_ctx->kthread_spares)
			cxl_flush_buf_free(flush_ctx->kthread_spares[cpu]);
	}
	kfree(flush_ctx->kthread_spares);
	free_percpu(flush_ctx->pcpu);
	flush_record_free(flush_ctx->flush_record, flush_ctx->flush_record_pages);
	return rc ? rc : -ENOMEM;
}

static int cxl_compression_attach_dvsec(struct cxl_memdev *cxlmd)
{
	struct pci_dev *pdev = to_pci_dev(cxlmd->dev.parent);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_endpoint_dvsec_info *info = &cxlds->dvsec_info;
	int i, rc;

	dev_info(&cxlmd->dev,
		 "DVSEC fallback: %d range(s) available\n", info->ranges);

	for (i = 0; i < info->ranges; i++) {
		struct range *hpa = &info->dvsec_range[i];

		if (range_len(hpa) == 0)
			continue;

		rc = convert_dvsec_to_sysram(hpa, pdev);
		if (rc) {
			dev_err(&cxlmd->dev,
				"DVSEC range %d conversion failed: %d\n",
				i, rc);
			return rc;
		}
	}

	return 0;
}

static int convert_region_to_sysram(struct cxl_region *cxlr,
				    struct pci_dev *pdev)
{
	struct cxl_compression_ctx *comp_ctx = pdev_to_comp_ctx(pdev);
	struct device *dev = cxl_region_dev(cxlr);
	struct cxl_compression_wm_ctx *wm_ctx;
	struct cxl_teardown_ctx *tctx;
	struct cxl_flush_ctx *flush_ctx;
	struct cxl_pcpu_flush *pcpu;
	resource_size_t region_start, region_size;
	struct range hpa_range;
	int nid;
	int irq;
	int cpu;
	int rc;

	if (cxl_region_mode(cxlr) != CXL_PARTMODE_RAM) {
		dev_dbg(dev, "skipping non-RAM region (mode=%d)\n",
			cxl_region_mode(cxlr));
		return 0;
	}

	dev_info(dev, "converting region to sysram\n");

	rc = devm_cxl_add_sysram(cxlr, true, MMOP_ONLINE_MOVABLE);
	if (rc) {
		dev_err(dev, "failed to add sysram region: %d\n", rc);
		return rc;
	}

	tctx = devm_kzalloc(dev, sizeof(*tctx), GFP_KERNEL);
	if (!tctx)
		return -ENOMEM;

	rc = devm_add_action_or_reset(dev, cxl_compression_post_teardown, tctx);
	if (rc)
		return rc;

	/* Find the sysram child device for pre_teardown */
	comp_ctx->sysram = cxl_region_find_sysram(cxlr);
	if (comp_ctx->sysram)
		tctx->sysram = comp_ctx->sysram;

	rc = cxl_get_region_range(cxlr, &hpa_range);
	if (rc) {
		dev_err(dev, "failed to get region range: %d\n", rc);
		return rc;
	}

	nid = phys_to_target_node(hpa_range.start);
	if (nid == NUMA_NO_NODE)
		nid = memory_add_physaddr_to_nid(hpa_range.start);

	region_start = hpa_range.start;
	region_size = range_len(&hpa_range);

	flush_ctx = devm_kzalloc(dev, sizeof(*flush_ctx), GFP_KERNEL);
	if (!flush_ctx)
		return -ENOMEM;

	flush_ctx->base_pfn = PHYS_PFN(region_start);
	flush_ctx->nr_pages = region_size >> PAGE_SHIFT;
	flush_ctx->flush_record = flush_record_alloc(flush_ctx->nr_pages,
						     &flush_ctx->flush_record_pages);
	if (!flush_ctx->flush_record)
		return -ENOMEM;

	flush_ctx->mbox = comp_ctx->mbox;
	flush_ctx->dev = dev;
	flush_ctx->nid = nid;
	flush_ctx->media_ops_supported = comp_ctx->media_ops_supported;

	/*
	 * Cap buffer at max DPA ranges that fit in one CCI payload.
	 * Header is 8 bytes (struct cxl_media_op_input), each range
	 * is 16 bytes (struct cxl_dpa_range).  The module parameter
	 * flush_buf_size can further limit this (0 = use hw max).
	 */
	flush_ctx->buf_max = (flush_ctx->mbox->payload_size -
			      sizeof(struct cxl_media_op_input)) /
			     sizeof(struct cxl_dpa_range);
	if (flush_buf_size && flush_buf_size < flush_ctx->buf_max)
		flush_ctx->buf_max = flush_buf_size;
	if (flush_ctx->buf_max == 0)
		flush_ctx->buf_max = 1;

	dev_info(dev,
		 "flush buffer: %u DPA ranges per command (payload %zu bytes, media_ops %s)\n",
		 flush_ctx->buf_max, flush_ctx->mbox->payload_size,
		 flush_ctx->media_ops_supported ? "yes" : "no");

	flush_ctx->pcpu = alloc_percpu(struct cxl_pcpu_flush);
	if (!flush_ctx->pcpu)
		return -ENOMEM;

	flush_ctx->kthread_spares = kcalloc(nr_cpu_ids,
					    sizeof(struct cxl_flush_buf *),
					    GFP_KERNEL);
	if (!flush_ctx->kthread_spares)
		goto err_pcpu_init;

	for_each_possible_cpu(cpu) {
		struct cxl_flush_buf *active_buf, *overflow_buf, *spare_buf;

		active_buf = cxl_flush_buf_alloc(flush_ctx->buf_max, nid);
		if (!active_buf)
			goto err_pcpu_init;

		overflow_buf = cxl_flush_buf_alloc(flush_ctx->buf_max, nid);
		if (!overflow_buf) {
			cxl_flush_buf_free(active_buf);
			goto err_pcpu_init;
		}

		spare_buf = cxl_flush_buf_alloc(flush_ctx->buf_max, nid);
		if (!spare_buf) {
			cxl_flush_buf_free(active_buf);
			cxl_flush_buf_free(overflow_buf);
			goto err_pcpu_init;
		}

		pcpu = per_cpu_ptr(flush_ctx->pcpu, cpu);
		pcpu->ctx = flush_ctx;
		rcu_assign_pointer(pcpu->active, active_buf);
		pcpu->overflow_spare = overflow_buf;
		INIT_WORK(&pcpu->overflow_work, cxl_flush_overflow_work);

		flush_ctx->kthread_spares[cpu] = spare_buf;
	}

	flush_ctx->flush_thread = kthread_create_on_node(
		cxl_flush_kthread_fn, flush_ctx, nid, "cxl-flush/%d", nid);
	if (IS_ERR(flush_ctx->flush_thread)) {
		rc = PTR_ERR(flush_ctx->flush_thread);
		flush_ctx->flush_thread = NULL;
		goto err_pcpu_init;
	}
	wake_up_process(flush_ctx->flush_thread);

	rc = cram_register_private_node(nid, cxlr,
					cxl_compression_flush_cb, flush_ctx);
	if (rc) {
		dev_err(dev, "failed to register cram node %d: %d\n", nid, rc);
		goto err_pcpu_init;
	}

	tctx->flush_ctx = flush_ctx;
	tctx->nid = nid;

	rc = devm_add_action_or_reset(dev, cxl_compression_pre_teardown, tctx);
	if (rc)
		return rc;

	comp_ctx->flush_ctx = flush_ctx;
	comp_ctx->tctx = tctx;
	comp_ctx->nid = nid;

	/*
	 * Register watermark IRQ handlers on &pdev->dev for
	 * MSI-X vector 12 (lthresh) and vector 13 (hthresh).
	 */
	wm_ctx = devm_kzalloc(&pdev->dev, sizeof(*wm_ctx), GFP_KERNEL);
	if (!wm_ctx)
		return -ENOMEM;

	wm_ctx->dev = &pdev->dev;
	wm_ctx->nid = nid;

	irq = pci_irq_vector(pdev, CXL_CT3_MSIX_LTHRESH);
	if (irq >= 0) {
		rc = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					       cxl_compression_lthresh_irq,
					       IRQF_ONESHOT,
					       "cxl-lthresh", wm_ctx);
		if (rc)
			dev_warn(&pdev->dev,
				 "failed to register lthresh IRQ: %d\n", rc);
	}

	irq = pci_irq_vector(pdev, CXL_CT3_MSIX_HTHRESH);
	if (irq >= 0) {
		rc = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					       cxl_compression_hthresh_irq,
					       IRQF_ONESHOT,
					       "cxl-hthresh", wm_ctx);
		if (rc)
			dev_warn(&pdev->dev,
				 "failed to register hthresh IRQ: %d\n", rc);
	}

	return 0;

err_pcpu_init:
	if (flush_ctx->flush_thread)
		kthread_stop(flush_ctx->flush_thread);
	for_each_possible_cpu(cpu) {
		struct cxl_flush_buf *buf;

		pcpu = per_cpu_ptr(flush_ctx->pcpu, cpu);

		buf = rcu_dereference_raw(pcpu->active);
		cxl_flush_buf_free(buf);

		cxl_flush_buf_free(pcpu->overflow_spare);

		if (flush_ctx->kthread_spares)
			cxl_flush_buf_free(flush_ctx->kthread_spares[cpu]);
	}
	kfree(flush_ctx->kthread_spares);
	free_percpu(flush_ctx->pcpu);
	flush_record_free(flush_ctx->flush_record, flush_ctx->flush_record_pages);
	return rc ? rc : -ENOMEM;
}

static struct cxl_region *create_ram_region(struct cxl_memdev *cxlmd)
{
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
		dev_info(&cxlmd->dev, "RAM capacity too small (< 256M)\n");
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

	dev_info(&cxlmd->dev, "created region %s\n",
		 dev_name(cxl_region_dev(cxlr)));
	pdev_to_comp_ctx(to_pci_dev(cxlmd->dev.parent))->cxled = cxled;
	return cxlr;
}

static int cxl_compression_attach_probe(struct cxl_memdev *cxlmd)
{
	struct pci_dev *pdev = to_pci_dev(cxlmd->dev.parent);
	struct cxl_compression_ctx *comp_ctx = pdev_to_comp_ctx(pdev);
	struct cxl_region *regions[8];
	struct cxl_region *cxlr;
	int nr, i, converted = 0, errors = 0;
	int rc;

	comp_ctx->cxlmd = cxlmd;
	comp_ctx->mbox = &cxlmd->cxlds->cxl_mbox;

	/* Probe device for media operations zero support */
	comp_ctx->media_ops_supported =
		cxl_probe_media_ops_zero(comp_ctx->mbox,
					 &cxlmd->dev);

	/* RCD fallback: no endpoint port, use DVSEC ranges directly */
	if (!cxlmd->endpoint)
		return cxl_compression_attach_dvsec(cxlmd);

	dev_info(&cxlmd->dev, "compression attach: looking for regions\n");

	nr = cxl_get_committed_regions(cxlmd, regions, ARRAY_SIZE(regions));
	for (i = 0; i < nr; i++) {
		if (cxl_region_mode(regions[i]) == CXL_PARTMODE_RAM) {
			rc = convert_region_to_sysram(regions[i], pdev);
			if (rc)
				errors++;
			else
				converted++;
		}
		put_device(cxl_region_dev(regions[i]));
	}

	if (converted > 0) {
		dev_info(&cxlmd->dev,
			 "converted %d regions to sysram (%d errors)\n",
			 converted, errors);
		return errors ? -EIO : 0;
	}

	dev_info(&cxlmd->dev, "no existing regions, creating RAM region\n");

	cxlr = create_ram_region(cxlmd);
	if (IS_ERR(cxlr)) {
		rc = PTR_ERR(cxlr);
		if (rc == -ENODEV) {
			dev_info(&cxlmd->dev,
				 "could not create RAM region: %d\n", rc);
			return 0;
		}
		return rc;
	}

	rc = convert_region_to_sysram(cxlr, pdev);
	if (rc) {
		dev_err(&cxlmd->dev,
			"failed to convert region to sysram: %d\n", rc);
		return rc;
	}

	comp_ctx->cxlr = cxlr;

	dev_info(&cxlmd->dev, "created and converted region %s to sysram\n",
		 dev_name(cxl_region_dev(cxlr)));

	return 0;
}

static const struct cxl_memdev_attach cxl_compression_attach = {
	.probe = cxl_compression_attach_probe,
};

static int cxl_compression_probe(struct pci_dev *pdev,
				 const struct pci_device_id *id)
{
	struct cxl_compression_ctx *comp_ctx;
	struct cxl_memdev *cxlmd;
	int rc;

	dev_info(&pdev->dev, "cxl_compression: probing device\n");

	comp_ctx = devm_kzalloc(&pdev->dev, sizeof(*comp_ctx), GFP_KERNEL);
	if (!comp_ctx)
		return -ENOMEM;
	comp_ctx->nid = NUMA_NO_NODE;
	comp_ctx->pdev = pdev;

	rc = xa_insert(&comp_ctx_xa, (unsigned long)pdev, comp_ctx, GFP_KERNEL);
	if (rc)
		return rc;

	cxlmd = cxl_pci_type3_probe_init(pdev, &cxl_compression_attach);
	if (IS_ERR(cxlmd)) {
		xa_erase(&comp_ctx_xa, (unsigned long)pdev);
		return PTR_ERR(cxlmd);
	}

	comp_ctx->cxlmd = cxlmd;
	comp_ctx->mbox = &cxlmd->cxlds->cxl_mbox;

	dev_info(&pdev->dev, "cxl_compression: probe complete\n");
	return 0;
}

static void cxl_compression_remove(struct pci_dev *pdev)
{
	struct cxl_compression_ctx *comp_ctx = xa_erase(&comp_ctx_xa,
							(unsigned long)pdev);

	dev_info(&pdev->dev, "cxl_compression: removing device\n");

	if (!comp_ctx || comp_ctx->nid == NUMA_NO_NODE)
		return;

	/*
	 * Destroy the region, devm actions on the region device handle teardown
	 * in registration-reverse order:
	 *   1. pre_teardown:  cram_unregister + retry-forever memory offline
	 *   2. sysram_unregister: device_unregister (sysram->res is NULL
	 *      after pre_teardown, so cxl_sysram_release skips hotplug)
	 *   3. post_teardown: kthread stop, flush cleanup
	 *
	 * PCI MMIO is still live so CCI commands in post_teardown work.
	 */
	if (comp_ctx->cxlr) {
		cxl_destroy_region(comp_ctx->cxlr);
		comp_ctx->cxlr = NULL;
	}

	if (comp_ctx->cxled) {
		cxl_dpa_free(comp_ctx->cxled);
		comp_ctx->cxled = NULL;
	}
}

static const struct pci_device_id cxl_compression_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x0d93) },
	{ /* terminate list */ },
};
MODULE_DEVICE_TABLE(pci, cxl_compression_pci_tbl);

static struct pci_driver cxl_compression_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= cxl_compression_pci_tbl,
	.probe		= cxl_compression_probe,
	.remove		= cxl_compression_remove,
	.driver	= {
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_pci_driver(cxl_compression_driver);

MODULE_DESCRIPTION("CXL: Compression Memory Driver with SysRAM regions");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS("CXL");
