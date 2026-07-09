// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM proactive-promotion (report_hot_pages) probe.
 *
 * Demotes a private-anon region onto the cram node, then plays the device: it
 * reports the region's pfns as "hot" through the dax cram_hot_pages knob and
 * asserts CRAM proactively promotes them back to DRAM -- the folios leave the
 * cram node while the mapping stays mapped (no write, reads never fault).
 *
 * Usage: cram_hot_tool <cram_nid> <path-to-cram_hot_pages>
 * Output: "promote_delta=<n> oncram_before=<p> oncram_after=<p>"
 * Exit:   0 pages promoted off cram (oncram dropped)
 *         3 region did not demote onto cram (environmental)
 *         1 reported hot but pages stayed on cram
 *         2 usage/setup error
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

#define CRAM_DBG      "/sys/kernel/debug/cram"
#define PROMOTE_COUNT CRAM_DBG "/promote_count"
#define PATTERN  0xAB
#define NR_PAGES 512		/* 2M region; fits one cram_hot_pages write */

static long page_size;

static long read_long(const char *path)
{
	char b[32];
	int fd = open(path, O_RDONLY);
	long v = -1;

	if (fd < 0)
		return -1;
	if (read(fd, b, sizeof(b) - 1) > 0)
		v = atol(b);
	close(fd);
	return v;
}

static uint64_t pagemap(void *v)
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

static unsigned long pfn_of(void *v)
{
	uint64_t e = pagemap(v);

	return (e & (1ULL << 63)) ? (e & ((1ULL << 55) - 1)) : 0;
}

static long on_node(void *v, int nid)
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

int main(int argc, char **argv)
{
	long len, before, after, oncram0, oncram1;
	unsigned char *p;
	char *buf;
	int nid, fd, i, sz = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <cram_nid> <cram_hot_pages path>\n", argv[0]);
		return 2;
	}
	nid = atoi(argv[1]);
	page_size = sysconf(_SC_PAGESIZE);
	len = (long)NR_PAGES * page_size;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(p, PATTERN, len);
	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");
	for (i = 0; i < NR_PAGES; i++)		/* read-fault back, RO, stays on cram */
		(void)((volatile unsigned char *)p)[(long)i * page_size];

	oncram0 = on_node(p, nid);
	if (oncram0 <= NR_PAGES / 2) {
		printf("oncram_before=%ld (did not demote)\n", oncram0);
		return 3;
	}

	/* Build the hot-pfn list from pagemap and report it through the dax knob. */
	buf = malloc(NR_PAGES * 24);
	if (!buf)
		return 2;
	for (i = 0; i < NR_PAGES; i++) {
		unsigned long pfn = pfn_of(p + (long)i * page_size);

		if (pfn)
			sz += sprintf(buf + sz, "%lu ", pfn);
	}

	before = read_long(PROMOTE_COUNT);
	fd = open(argv[2], O_WRONLY);
	if (fd < 0) {
		perror("open cram_hot_pages");
		return 2;
	}
	if (write(fd, buf, sz) < 0)
		perror("write cram_hot_pages");
	close(fd);
	free(buf);

	/* Promotion is async (worker); give it a moment, keep the mapping alive. */
	for (i = 0; i < 50; i++) {
		usleep(100000);
		if (on_node(p, nid) <= NR_PAGES / 4)
			break;
	}
	after = read_long(PROMOTE_COUNT);
	oncram1 = on_node(p, nid);

	/* still readable after promotion? */
	if (((volatile unsigned char *)p)[0] != PATTERN)
		fprintf(stderr, "data corrupted after promotion\n");

	printf("promote_delta=%ld oncram_before=%ld oncram_after=%ld\n",
	       (before >= 0 && after >= 0) ? after - before : -1, oncram0, oncram1);

	munmap(p, len);
	return oncram1 <= NR_PAGES / 2 ? 0 : 1;
}
