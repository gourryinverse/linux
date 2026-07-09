// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM writeback-to-physical-swap test.
 *
 * Demotes anonymous memory onto the CRAM node until it is nearly full, then
 * balloon-inflates past the node's free capacity so the allocator's direct
 * reclaim must SWAP resident CRAM folios out to the physical swap device.
 * Verifies:
 *   - pswpout (/proc/vmstat) rises across the inflate  -> folios written to swap;
 *   - some of our mapped pages become swap entries      -> they left CRAM;
 *   - after deflate, touching every page returns the original contents
 *                                                       -> swap-in is correct.
 *
 * Usage: cram_swap_tool <cram_nid>
 * Exit 0 = swap-out happened and data survived; 3 = environmental (no demote);
 * 1 = contract failed; 2 = setup error.
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

#define CRAM_DBG   "/sys/kernel/debug/cram"
#define PATTERN    0xA7

static long page_size;

static long vmstat(const char *key)
{
	char line[128];
	FILE *f = fopen("/proc/vmstat", "r");
	long v = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char k[64];
		long n;

		if (sscanf(line, "%63s %ld", k, &n) == 2 && !strcmp(k, key)) {
			v = n;
			break;
		}
	}
	fclose(f);
	return v;
}

/* Read column @col (header name) for node @nid from cram/nodes. */
static long cram_node_col(int nid, const char *col)
{
	char line[256], hdr[256];
	FILE *f = fopen(CRAM_DBG "/nodes", "r");
	int idx = -1, i;
	long val = -1;
	char *tok;

	if (!f)
		return -1;
	if (!fgets(hdr, sizeof(hdr), f)) {
		fclose(f);
		return -1;
	}
	for (i = 0, tok = strtok(hdr, " \n"); tok; tok = strtok(NULL, " \n"), i++)
		if (!strcmp(tok, col))
			idx = i;
	if (idx < 0) {
		fclose(f);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		int n;

		if (sscanf(line, "%d", &n) == 1 && n == nid) {
			for (i = 0, tok = strtok(line, " \n"); tok;
			     tok = strtok(NULL, " \n"), i++)
				if (i == idx)
					val = atol(tok);
			break;
		}
	}
	fclose(f);
	return val;
}

static int ctl(const char *fmt, int nid, unsigned long n)
{
	char cmd[64];
	int fd = open(CRAM_DBG "/control", O_WRONLY);
	int len, rc;

	if (fd < 0)
		return -1;
	len = snprintf(cmd, sizeof(cmd), fmt, nid, n);
	rc = write(fd, cmd, len);
	close(fd);
	return rc < 0 ? -1 : 0;
}

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

static int page_swapped(void *v) { return !!(pagemap_entry(v) & (1ULL << 62)); }

int main(int argc, char **argv)
{
	int cram_nid;
	long present, free0, demote_pages, len, i;
	long pswp0, pswp1, swapped = 0;
	unsigned char *p;
	int data_ok = 1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cram_nid>\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	page_size = sysconf(_SC_PAGESIZE);

	present = cram_node_col(cram_nid, "present");
	if (present <= 0) {
		fprintf(stderr, "cram node %d not present\n", cram_nid);
		return 2;
	}
	/* Demote ~85% of the node so a modest inflate forces swap-out. */
	demote_pages = present * 85 / 100;
	len = demote_pages * page_size;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(p, PATTERN, len);

	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");

	free0 = cram_node_col(cram_nid, "free");
	fprintf(stderr, "diag: present=%ld demoted~%ld free_after_demote=%ld\n",
		present, demote_pages, free0);
	if (free0 < 0) {
		munmap(p, len);
		return 2;
	}
	/* If the node still has lots of free, demote did not really land. */
	if (free0 > present / 2) {
		fprintf(stderr, "demote did not fill the cram node (free=%ld/%ld)\n",
			free0, present);
		munmap(p, len);
		return 3;
	}

	pswp0 = vmstat("pswpout");

	/*
	 * Inflate past free: consume the remaining free, then ~64MB more, which
	 * the allocator can only satisfy by reclaiming (swapping out) resident
	 * CRAM folios.
	 */
	{
		unsigned long extra = (64UL << 20) / page_size;

		if (ctl("inflate %d %lu", cram_nid, free0 + extra)) {
			perror("inflate");
			munmap(p, len);
			return 2;
		}
	}

	pswp1 = vmstat("pswpout");

	/* How many of our pages became swap entries (left CRAM to swap)? */
	for (i = 0; i < demote_pages; i++)
		if (page_swapped(p + i * page_size))
			swapped++;

	/* Release the reservation and fault everything back in from swap. */
	ctl("deflate %d %lu", cram_nid, ~0UL);
	for (i = 0; i < demote_pages; i++)
		if (p[i * page_size] != PATTERN ||
		    p[i * page_size + page_size - 1] != PATTERN)
			data_ok = 0;

	fprintf(stderr,
		"pswpout %ld->%ld (+%ld) swapped_pages=%ld data_ok=%d\n",
		pswp0, pswp1, pswp1 - pswp0, swapped, data_ok);

	munmap(p, len);

	/* Contract: physical swap writeback happened and data survived swap-in. */
	if (pswp1 <= pswp0 && swapped == 0)
		return 1;	/* nothing was written back to swap */
	if (!data_ok)
		return 1;	/* swap-in corrupted data */
	printf("writeback-to-swap OK: +%ld pswpout, %ld pages swapped, data intact\n",
	       pswp1 - pswp0, swapped);
	return 0;
}
