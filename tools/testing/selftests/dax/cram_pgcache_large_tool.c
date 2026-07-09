// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM resident file tier -- LARGE (PMD) folio test.
 *
 * Forms a PMD-mapped file THP, demotes it resident onto the CRAM node, and
 * verifies the order-N contract:
 *   - the large folio goes resident on CRAM and re-maps PMD read-only in place
 *     (FilePmdMapped > 0, present, on-cram)           -> order-N read-in-place;
 *   - a read does not promote it (pfn/oncram unchanged);
 *   - a MAP_SHARED store promotes it off CRAM (the PMD was read-only: the write
 *     faults through wp_huge_pmd -> zap -> do_shared_fault -> filemap_fault
 *     FGP_WRITE promote), pfn moves and it leaves the CRAM node.
 *
 * Usage: cram_pgcache_large_tool <cram_nid> <mnt_dir>
 * Exit 0 contract held, 1 violation, 3 environmental (no PMD THP formed / not
 * resident), 2 setup error.
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
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define HPAGE (2UL * 1024 * 1024)	/* x86-64 PMD size */
#define NR_HP 4				/* 8 MiB file: a few THPs */
#define PATTERN 0xAB
#define NEWVAL  0xCD

static long page_size;
static int cram_nid;

static uint64_t pagemap_entry(void *v)
{
	uint64_t val = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);
	off_t off = ((uintptr_t)v / page_size) * sizeof(uint64_t);

	if (fd < 0)
		return 0;
	if (pread(fd, &val, sizeof(val), off) != sizeof(val))
		val = 0;
	close(fd);
	return val;
}
static int page_present(void *v) { return !!(pagemap_entry(v) & (1ULL << 63)); }
static uint64_t page_pfn(void *v)
{
	uint64_t e = pagemap_entry(v);

	return (e & (1ULL << 63)) ? (e & ((1ULL << 55) - 1)) : 0;
}
static long pages_on_node(void *v, int nid)
{
	char line[8192], key[32], tok[32];
	FILE *f = fopen("/proc/self/numa_maps", "r");
	long pages = -1;

	if (!f)
		return -1;
	snprintf(key, sizeof(key), "%lx", (unsigned long)v);
	snprintf(tok, sizeof(tok), "N%d=", nid);
	while (fgets(line, sizeof(line), f)) {
		char *p;

		if (strncmp(line, key, strlen(key)))
			continue;
		p = strstr(line, tok);
		pages = p ? atol(p + strlen(tok)) : 0;
		break;
	}
	fclose(f);
	return pages;
}

/* KiB value of @key in the /proc/self/smaps block for the VMA at @vaddr. */
static long smaps_kb(void *vaddr, const char *key)
{
	char line[256], want[64];
	FILE *f = fopen("/proc/self/smaps", "r");
	unsigned long lo = 0, hi = 0, target = (unsigned long)vaddr;
	int in = 0;
	long val = -1;

	if (!f)
		return -1;
	snprintf(want, sizeof(want), "%s:", key);
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
			in = (target >= lo && target < hi);
			continue;
		}
		if (in && !strncmp(line, want, strlen(want))) {
			val = atol(line + strlen(want));
			break;
		}
	}
	fclose(f);
	return val;
}

static int make_file(const char *path, unsigned char v, long bytes)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	void *buf;
	long off;

	if (fd < 0)
		return -1;
	buf = malloc(HPAGE);
	if (!buf) {
		close(fd);
		return -1;
	}
	memset(buf, v, HPAGE);
	for (off = 0; off < bytes; off += HPAGE)
		if (pwrite(fd, buf, HPAGE, off) != (ssize_t)HPAGE) {
			free(buf);
			close(fd);
			return -1;
		}
	free(buf);
	fsync(fd);
	return fd;
}

/* mmap the file at a 2 MiB-aligned address so a PMD mapping is possible. */
static unsigned char *map_aligned(int fd, long len, int prot)
{
	unsigned char *raw = mmap(NULL, len + HPAGE, PROT_NONE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char *aligned;

	if (raw == MAP_FAILED)
		return NULL;
	aligned = (unsigned char *)(((uintptr_t)raw + HPAGE - 1) & ~(HPAGE - 1));
	munmap(raw, len + HPAGE);
	aligned = mmap(aligned, len, prot, MAP_SHARED | MAP_FIXED, fd, 0);
	return aligned == MAP_FAILED ? NULL : aligned;
}

int main(int argc, char **argv)
{
	const char *mnt;
	char path[256];
	long len = (long)NR_HP * HPAGE, resident, oncram_before, oncram_after;
	unsigned char *p;
	uint64_t pfn_before;
	int fd, rc = 0;
	volatile unsigned char sink = 0;
	long i;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <cram_nid> <mnt_dir>\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	mnt = argv[2];
	page_size = sysconf(_SC_PAGESIZE);

	/* ---- form a PMD THP + demote it resident onto CRAM ---- */
	snprintf(path, sizeof(path), "%s/cram_pc_large", mnt);
	fd = make_file(path, PATTERN, len);
	if (fd < 0) { fprintf(stderr, "make_file\n"); return 2; }
	p = map_aligned(fd, len, PROT_READ);
	if (!p) { fprintf(stderr, "map_aligned\n"); close(fd); return 2; }
	madvise(p, len, MADV_HUGEPAGE);
	for (i = 0; i < len; i += page_size)
		sink += p[i];
	(void)sink;
	if (smaps_kb(p, "FilePmdMapped") <= 0) {
		fprintf(stderr, "env: no file PMD THP formed (FilePmdMapped=%ld); "
			"try CRAM_FS=xfs\n", smaps_kb(p, "FilePmdMapped"));
		munmap(p, len); close(fd);
		return 3;
	}
	if (madvise(p, len, MADV_PAGEOUT)) { munmap(p, len); close(fd); return 2; }
	sink += p[0];					/* re-fault the first THP */
	resident = pages_on_node(p, cram_nid);
	if (resident <= 0 || smaps_kb(p, "FilePmdMapped") <= 0) {
		fprintf(stderr, "env: large folio not resident PMD-mapped on CRAM "
			"(oncram=%ld pmd=%ldkB)\n", resident, smaps_kb(p, "FilePmdMapped"));
		munmap(p, len); close(fd);
		return 3;
	}
	printf("large-resident: oncram=%ld FilePmdMapped=%ldkB present=%d\n",
	       resident, smaps_kb(p, "FilePmdMapped"), page_present(&p[0]));

	/* ---- read must not promote a large resident folio ---- */
	oncram_before = pages_on_node(p, cram_nid);
	pfn_before = page_pfn(&p[0]);
	if (p[0] != PATTERN) { fprintf(stderr, "L1 wrong bytes\n"); rc = 1; }
	oncram_after = pages_on_node(p, cram_nid);
	if (page_pfn(&p[0]) != pfn_before || oncram_after < oncram_before) {
		fprintf(stderr, "L1 read promoted the large folio (oncram %ld->%ld)\n",
			oncram_before, oncram_after);
		rc = 1;
	}
	printf("L1 large-read-in-place: oncram %ld->%ld %s\n",
	       oncram_before, oncram_after, rc ? "FAIL" : "ok");
	munmap(p, len);
	close(fd);

	/*
	 * ---- MAP_SHARED store promotes the large folio off CRAM ----
	 * Fresh file: reusing the L1 file would leave its folios already resident
	 * on CRAM, so MADV_PAGEOUT would DROP them (clean folios drop, not
	 * re-demote) rather than demote a fresh THP.
	 */
	snprintf(path, sizeof(path), "%s/cram_pc_large2", mnt);
	fd = make_file(path, PATTERN, len);
	if (fd < 0) return rc ? 1 : 2;
	p = map_aligned(fd, len, PROT_READ | PROT_WRITE);
	if (!p) { close(fd); return rc ? 1 : 2; }
	madvise(p, len, MADV_HUGEPAGE);
	for (i = 0; i < len; i += page_size)
		sink += p[i];
	(void)sink;
	if (madvise(p, len, MADV_PAGEOUT)) { munmap(p, len); close(fd); return rc ? 1 : 2; }
	sink += p[0];
	resident = pages_on_node(p, cram_nid);
	if (resident <= 0) { munmap(p, len); close(fd); return rc ? 1 : 3; }
	{
		int was_pmd = smaps_kb(p, "FilePmdMapped") > 0;

		oncram_before = pages_on_node(p, cram_nid);
		pfn_before = page_pfn(&p[0]);
		p[0] = NEWVAL;				/* store -> promote off CRAM */
		oncram_after = pages_on_node(p, cram_nid);
		if (p[0] != NEWVAL) { fprintf(stderr, "L2 store no effect\n"); rc = 1; }
		if (page_pfn(&p[0]) == pfn_before || oncram_after >= oncram_before) {
			fprintf(stderr, "L2 shared write did NOT promote off CRAM "
				"(pfn %llx->%llx oncram %ld->%ld) -- IN-PLACE DEVICE WRITE\n",
				(unsigned long long)pfn_before,
				(unsigned long long)page_pfn(&p[0]),
				oncram_before, oncram_after);
			rc = 1;
		}
		printf("L2 large-shared-write-promote: was_pmd=%d oncram %ld->%ld %s\n",
		       was_pmd, oncram_before, oncram_after, rc ? "FAIL" : "ok");
	}
	munmap(p, len);
	close(fd);
	return rc;
}
