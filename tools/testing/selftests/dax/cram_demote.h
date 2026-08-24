/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cram_demote.h - put a named range on the CRAM tier.
 *
 * CRAM used to run its own demotion pass inside shrink_folio_list(), and
 * MADV_PAGEOUT reached it: the divert ran before add_to_swap() and never
 * consulted sc->no_demotion.  CRAM now rides the generic tiering pass, where
 * MADV_PAGEOUT cannot demote at all - its backend, reclaim_folio_list(), sets
 * .no_demotion = 1, so can_demote() is false for every node.
 *
 * Nor can cgroup memory.reclaim stand in.  It does demote, but it is
 * charge-scoped rather than range-scoped: reclaiming a 1MB region moves
 * nothing, and marking a small region MADV_COLD before reclaiming a 400MB
 * ballast demotes the ballast and leaves the region where it was.  Every test
 * here needs the opposite - place THESE pages, then assert what happens when
 * they are read or written.
 *
 * So placement goes through the [TEST] debugfs verb, which chooses folios and
 * hands them to the same alloc_migration_target() path reclaim uses.  When the
 * tests can drive the tier through memory.reclaim alone, this goes away with
 * the verb.
 */
#ifndef __CRAM_DEMOTE_H__
#define __CRAM_DEMOTE_H__

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MADV_COLD
#define MADV_COLD 20
#endif

#define CRAM_DBG_DIR	"/sys/kernel/debug/cram"
#ifndef CRAM_CONTROL
#define CRAM_CONTROL	CRAM_DBG_DIR "/control"
#endif
#define CRAM_NODES	CRAM_DBG_DIR "/nodes"

/**
 * cram_first_node - the first node CRAM manages, or -1
 *
 * Read from debugfs rather than taken as an argument so a caller does not have
 * to thread the id through; every test in this suite drives one CRAM node.
 */
static inline int cram_first_node(void)
{
	char line[256];
	FILE *f = fopen(CRAM_NODES, "r");
	int nid = -1;

	if (!f)
		return -1;
	/* header, then one line per node: "node zratio ... free present" */
	if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f))
		nid = atoi(line);
	fclose(f);
	return nid;
}

/**
 * cram_demote - place [@addr,@addr+@len) on the CRAM node
 *
 * Returns 0 if the range was handed to the verb, -1 otherwise, matching the
 * madvise() convention it replaces so callers test it the same way.
 *
 * One write covers the whole range: the kernel walks the caller's page tables
 * itself.  Writing a pfn per page instead made the re-demote loops in the
 * torture tests time out, which then left the dax device unbindable for every
 * test that ran after them.
 *
 * The verb refuses anything the tier would refuse, so a page that does not
 * appear on the node afterwards was rejected by the placement contract.
 */
static inline int cram_demote(void *addr, size_t len)
{
	long ps = sysconf(_SC_PAGESIZE);
	char cmd[96];
	int ctl, nid;
	ssize_t rc;

	nid = cram_first_node();
	if (nid < 0)
		return -1;

	ctl = open(CRAM_CONTROL, O_WRONLY);
	if (ctl < 0)
		return -1;

	rc = snprintf(cmd, sizeof(cmd), "demote %d %lu %lu", nid,
		      (unsigned long)addr, (unsigned long)(len / ps));
	rc = write(ctl, cmd, rc);
	close(ctl);
	if (rc <= 0)
		return -1;

	/*
	 * Leave the range cold.  MADV_PAGEOUT both aged and reclaimed; the verb
	 * only places.  Folios left young are activated by
	 * folio_check_references() on the next scan instead of being reclaimed,
	 * which is correct mm behaviour but means a test that demotes and then
	 * expects writeback measures nothing: reclaim scanned 194969 folios,
	 * stole 0 and activated all of them.
	 */
	madvise(addr, len, MADV_COLD);
	return 0;
}


/*
 * Demotion and promotion counters.
 *
 * CRAM does not count these; core mm does, per node.  The cram debugfs
 * demote_count/promote_count files these tests used to read were removed when
 * demotion and promotion were generalised, and every read had been silently
 * returning -1 since.
 *
 * Demotion is counted against the node the folio came FROM, so a test asking
 * "did anything demote" has to sum across nodes -- reading the cram node's
 * pgdemote_* gives zero no matter how much landed there.  Promotion off a
 * write-fenced node is counted against that node, so it is read per-nid.
 */
static inline long cram_node_vmstat(int nid, const char *key)
{
	char path[64], line[128];
	long val = -1;
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/node/node%d/vmstat", nid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char k[64];
		long v;

		if (sscanf(line, "%63s %ld", k, &v) == 2 && !strcmp(k, key)) {
			val = v;
			break;
		}
	}
	fclose(f);
	return val;
}

/* Total pages demoted anywhere, all sources.  -1 if nothing readable. */
static inline long cram_demoted(void)
{
	static const char * const keys[] = {
		"pgdemote_kswapd", "pgdemote_direct",
		"pgdemote_khugepaged", "pgdemote_proactive",
	};
	long total = -1;
	int nid, k;

	for (nid = 0; nid < 16; nid++) {
		for (k = 0; k < 4; k++) {
			long v = cram_node_vmstat(nid, keys[k]);

			if (v < 0)
				continue;
			total = (total < 0) ? v : total + v;
		}
	}
	return total;
}

/* Folios promoted off write-fenced node @nid, and failures. */
static inline long cram_promoted(int nid)
{
	return cram_node_vmstat(nid, "pgpromote_fence");
}

static inline long cram_promote_failed(int nid)
{
	return cram_node_vmstat(nid, "pgpromote_fence_failed");
}

#endif /* __CRAM_DEMOTE_H__ */
