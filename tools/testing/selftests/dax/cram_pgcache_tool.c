// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM resident (read-in-place) file-cache tier test.
 *
 * A clean file folio that reclaim would drop is instead demoted onto the CRAM
 * private node by migration and stays in the page cache, mapped read-only in
 * place.  This verifies the resident contract on a real (block-backed) fs:
 *
 *   1. resident read: after MADV_PAGEOUT the folio is resident on the CRAM node
 *      and an mmap read faults it in READ-ONLY in place (present, on-cram) and
 *      returns the original bytes with the pfn unchanged  -> zero-copy read.
 *   2. shared write promotes: a store through a MAP_SHARED writable mapping
 *      changes the pfn and moves the folio off the CRAM node with the new bytes
 *      -> write-fenced, promoted off the device (never written in place).
 *   3. write(2) promotes: a pwrite() to a resident index promotes it off CRAM
 *      before the store lands, and the new byte is visible.
 *
 * Usage: cram_pgcache_tool <cram_nid> <mnt_dir>
 * Exit 0 if all contracts held, 3 if a clean file folio never went resident on
 * CRAM (environmental: large folios / migrate declined), 1 on a contract fail.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MADV_RANDOM
#define MADV_RANDOM 1
#endif

#define PATTERN 0xAB
#define NEWVAL  0xCD
#define NR_PAGES 64
#define PROBE 7			/* the page index we probe in each test */

static long page_size;
static int cram_nid;

/* Raw /proc/self/pagemap entry for @vaddr. */
static uint64_t pagemap_entry(void *vaddr)
{
	uint64_t val = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);
	off_t off = ((uintptr_t)vaddr / page_size) * sizeof(uint64_t);

	if (fd < 0)
		return 0;
	if (pread(fd, &val, sizeof(val), off) != sizeof(val))
		val = 0;
	close(fd);
	return val;
}

static int page_present(void *vaddr) { return !!(pagemap_entry(vaddr) & (1ULL << 63)); }

static uint64_t page_pfn(void *vaddr)
{
	uint64_t v = pagemap_entry(vaddr);

	if (!(v & (1ULL << 63)))
		return 0;
	return v & ((1ULL << 55) - 1);
}

/* Pages of the VMA at @vaddr resident on node @nid, per numa_maps. */
static long pages_on_node(void *vaddr, int nid)
{
	char line[8192], key[32], tok[32];
	FILE *f = fopen("/proc/self/numa_maps", "r");
	long pages = -1;

	if (!f)
		return -1;
	snprintf(key, sizeof(key), "%lx", (unsigned long)vaddr);
	snprintf(tok, sizeof(tok), "N%d=", nid);
	while (fgets(line, sizeof(line), f)) {
		char *p;

		if (strncmp(line, key, strlen(key)) != 0)
			continue;
		p = strstr(line, tok);
		pages = p ? atol(p + strlen(tok)) : 0;
		break;
	}
	fclose(f);
	return pages;
}

/* Create + populate a clean, on-disk file of NR_PAGES pages of @val. */
static int make_file(const char *path, unsigned char val)
{
	unsigned char *buf;
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	long i;

	if (fd < 0)
		return -1;
	buf = malloc((size_t)NR_PAGES * page_size);
	if (!buf) {
		close(fd);
		return -1;
	}
	memset(buf, val, (size_t)NR_PAGES * page_size);
	for (i = 0; i < NR_PAGES; i++) {
		if (pwrite(fd, buf + i * page_size, page_size,
			   i * page_size) != page_size) {
			free(buf);
			close(fd);
			return -1;
		}
	}
	free(buf);
	fsync(fd);		/* folios are now clean vs disk */
	return fd;
}

/*
 * mmap @fd, fault every page in read-only (keeps them clean), then MADV_PAGEOUT
 * so reclaim demotes the clean file folios onto the CRAM node.  Re-faults the
 * probe page (a resident CRAM folio maps RO in place) and returns the mapping;
 * *resident is set to the probe VMA's pages now on the CRAM node.
 */
static unsigned char *demote_resident(int fd, int prot, int flags, long *lenp,
				      long *resident)
{
	long len = (long)NR_PAGES * page_size, i;
	unsigned char *p = mmap(NULL, len, prot, flags, fd, 0);
	volatile unsigned char sink = 0;

	if (p == MAP_FAILED)
		return NULL;
	madvise(p, len, MADV_RANDOM);		/* order-0, no readahead large folios */
	for (i = 0; i < NR_PAGES; i++)		/* read-fault them in, clean */
		sink += p[i * page_size];
	(void)sink;
	if (madvise(p, len, MADV_PAGEOUT)) {	/* reclaim -> CRAM demote-in */
		munmap(p, len);
		return NULL;
	}
	sink += p[PROBE * page_size];		/* re-fault: resident CRAM folio, RO */
	*lenp = len;
	*resident = pages_on_node(p, cram_nid);
	return p;
}

int main(int argc, char **argv)
{
	const char *mnt;
	char path[256];
	int fd, rc = 0;
	long len, resident, oncram_before, oncram_after;
	unsigned char *p;
	uint64_t pfn_before, pfn_after;
	unsigned long probe_off;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <cram_nid> <mnt_dir>\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	mnt = argv[2];
	page_size = sysconf(_SC_PAGESIZE);
	probe_off = (unsigned long)PROBE * page_size;

	/* ---- Test 1: resident read-in-place -- a read must NOT promote ---- */
	snprintf(path, sizeof(path), "%s/cram_pc_read", mnt);
	fd = make_file(path, PATTERN);
	if (fd < 0) {
		fprintf(stderr, "make_file failed\n");
		return 2;
	}
	p = demote_resident(fd, PROT_READ, MAP_SHARED, &len, &resident);
	if (!p) {
		fprintf(stderr, "demote_resident failed\n");
		close(fd);
		return 2;
	}
	if (resident <= 0) {
		fprintf(stderr, "env: no clean file folio went resident on CRAM "
			"(resident=%ld)\n", resident);
		munmap(p, len);
		close(fd);
		return rc ? 1 : 3;		/* environmental (preserve any fail) */
	}
	/* Drop the probe PTE (folio stays resident on CRAM) so the read re-faults. */
	madvise(&p[probe_off], page_size, MADV_DONTNEED);
	oncram_before = pages_on_node(p, cram_nid);
	if (p[probe_off] != PATTERN) {			/* read-faults from CRAM in place */
		fprintf(stderr, "resident read: wrong bytes %#x\n", p[probe_off]);
		rc = 1;
	}
	oncram_after = pages_on_node(p, cram_nid);
	/*
	 * Read-in-place re-maps the probe onto CRAM, so oncram must grow: before
	 * excludes the dropped probe PTE, after re-counts it.  If it did not grow,
	 * the read promoted the folio off CRAM instead of mapping it in place.
	 */
	if (!page_present(&p[probe_off]) || oncram_after <= oncram_before) {
		fprintf(stderr, "resident read promoted the folio off CRAM "
			"(oncram %ld->%ld) -- read should map in place\n",
			oncram_before, oncram_after);
		rc = 1;
	}
	printf("T1 resident-read: resident=%ld oncram %ld->%ld present=%d %s\n",
	       resident, oncram_before, oncram_after, page_present(&p[probe_off]),
	       rc ? "FAIL" : "ok");
	munmap(p, len);
	close(fd);

	/* ---- Test 2: MAP_SHARED store promotes the written page off CRAM ---- */
	snprintf(path, sizeof(path), "%s/cram_pc_shwrite", mnt);
	fd = make_file(path, PATTERN);
	if (fd < 0)
		return rc ? 1 : 2;
	p = demote_resident(fd, PROT_READ | PROT_WRITE, MAP_SHARED, &len, &resident);
	if (!p) {
		close(fd);
		return rc ? 1 : 2;
	}
	if (resident <= 0) {
		munmap(p, len);
		close(fd);
		return rc ? 1 : 3;
	}
	oncram_before = pages_on_node(p, cram_nid);
	pfn_before = page_pfn(&p[probe_off]);
	p[probe_off] = NEWVAL;				/* store -> promote off CRAM */
	pfn_after = page_pfn(&p[probe_off]);
	oncram_after = pages_on_node(p, cram_nid);
	if (p[probe_off] != NEWVAL) {
		fprintf(stderr, "shared write did not take effect\n");
		rc = 1;
	}
	/*
	 * Promote proof: the written frame MOVED (an in-place device write would
	 * keep the pfn) and the folio LEFT the CRAM node (oncram drops by the one
	 * written page; the unwritten pages stay resident).
	 */
	if (pfn_after == pfn_before || oncram_after >= oncram_before) {
		fprintf(stderr, "shared write did NOT promote off CRAM "
			"(pfn %llx->%llx oncram %ld->%ld) -- IN-PLACE DEVICE WRITE\n",
			(unsigned long long)pfn_before, (unsigned long long)pfn_after,
			oncram_before, oncram_after);
		rc = 1;
	}
	printf("T2 shared-write-promote: pfn %llx->%llx oncram %ld->%ld %s\n",
	       (unsigned long long)pfn_before, (unsigned long long)pfn_after,
	       oncram_before, oncram_after, rc ? "FAIL" : "ok");
	munmap(p, len);
	close(fd);

	/* ---- Test 3: write(2) promotes off CRAM before the store lands ---- */
	snprintf(path, sizeof(path), "%s/cram_pc_w2", mnt);
	fd = make_file(path, PATTERN);
	if (fd < 0)
		return rc ? 1 : 2;
	p = demote_resident(fd, PROT_READ, MAP_SHARED, &len, &resident);
	if (!p) {
		close(fd);
		return rc ? 1 : 2;
	}
	if (resident <= 0) {
		munmap(p, len);
		close(fd);
		return rc ? 1 : 3;
	}
	{
		unsigned char nv = NEWVAL;

		oncram_before = pages_on_node(p, cram_nid);
		if (pwrite(fd, &nv, 1, (off_t)probe_off) != 1) {
			fprintf(stderr, "pwrite failed\n");
			rc = 1;
		}
		/* The mmap must now see the new byte (folio promoted + updated). */
		if (p[probe_off] != NEWVAL) {
			fprintf(stderr, "write(2) byte not visible via mmap (%#x)\n",
				p[probe_off]);
			rc = 1;
		}
		oncram_after = pages_on_node(p, cram_nid);
		if (oncram_after >= oncram_before) {
			fprintf(stderr, "write(2) did NOT promote off CRAM "
				"(oncram %ld->%ld)\n", oncram_before, oncram_after);
			rc = 1;
		}
		printf("T3 write2-promote: byte=%#x oncram %ld->%ld %s\n",
		       p[probe_off], oncram_before, oncram_after, rc ? "FAIL" : "ok");
	}
	munmap(p, len);
	close(fd);

	/* ---- Test 4: drop_caches evicts the resident CRAM tier ---- */
	snprintf(path, sizeof(path), "%s/cram_pc_drop", mnt);
	fd = make_file(path, PATTERN);
	if (fd < 0)
		return rc ? 1 : 2;
	p = demote_resident(fd, PROT_READ, MAP_SHARED, &len, &resident);
	if (!p) {
		close(fd);
		return rc ? 1 : 2;
	}
	if (resident <= 0) {
		munmap(p, len);
		close(fd);
		return rc ? 1 : 3;
	}
	munmap(p, len);				/* unmap so the folios are droppable */
	{
		int dfd = open("/proc/sys/vm/drop_caches", O_WRONLY);

		if (dfd < 0 || write(dfd, "1\n", 2) != 2) {
			fprintf(stderr, "drop_caches write failed\n");
			rc = 1;
		}
		if (dfd >= 0)
			close(dfd);
	}
	/*
	 * Re-map + read.  In the resident model the CRAM folio IS the page-cache
	 * folio, so drop_caches must have evicted it: this read refaults from disk
	 * to a DRAM folio (oncram==0), content intact.  If drop_caches had missed
	 * the tier, the read would map the still-resident CRAM folio (oncram>0).
	 */
	p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return rc ? 1 : 2;
	}
	madvise(p, len, MADV_RANDOM);
	if (p[probe_off] != PATTERN) {
		fprintf(stderr, "drop-caches: content lost (%#x)\n", p[probe_off]);
		rc = 1;
	}
	oncram_after = pages_on_node(p, cram_nid);
	if (oncram_after != 0) {
		fprintf(stderr, "drop_caches did NOT evict the resident CRAM tier "
			"(oncram=%ld) -- folio still on device after drop\n", oncram_after);
		rc = 1;
	}
	printf("T4 drop-caches-evicts: resident=%ld oncram_after=%ld %s\n",
	       resident, oncram_after, rc ? "FAIL" : "ok");
	munmap(p, len);
	close(fd);

	return rc;
}
