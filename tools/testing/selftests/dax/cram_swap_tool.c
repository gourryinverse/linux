// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM writeback-to-physical-swap test.
 *
 * Demotes anonymous memory onto the CRAM node until it is nearly full, then
 * balloon-inflates past the node's free capacity so the allocator's direct
 * reclaim must SWAP resident CRAM folios out to the physical swap device.
 * Verifies:
 *   - some of our mapped pages become swap entries      -> they left CRAM;
 *   - after deflate, touching every page returns the original contents
 *                                                       -> swap-in is correct.
 *
 * pswpout is reported as a diagnostic, but need not rise: MADV_PAGEOUT can
 * leave a resident CRAM folio with valid swap backing, allowing later reclaim
 * to install the existing swap entry without another write.
 *
 * Usage: cram_swap_tool <cram_nid> <balloon-target-path> <no-alloc-path>
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
#include "cram_demote.h"

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

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

static long node_present_pages(int nid)
{
	char line[256], path[128];
	long kb = -1;
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/node/node%d/meminfo", nid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "Node %*d MemTotal: %ld kB", &kb) == 1)
			break;
	}
	fclose(f);
	return kb < 0 ? -1 : kb * 1024 / page_size;
}

static long node_vmstat(int nid, const char *name)
{
	char key[64], line[256], path[128];
	long val = -1;
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/node/node%d/vmstat", nid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		long value;

		if (sscanf(line, "%63s %ld", key, &value) == 2 &&
		    !strcmp(key, name)) {
			val = value;
			break;
		}
	}
	fclose(f);
	return val;
}

static int set_value(const char *path, unsigned long value)
{
	char cmd[64];
	int fd = open(path, O_WRONLY);
	int len, rc;

	if (fd < 0)
		return -1;
	len = snprintf(cmd, sizeof(cmd), "%lu", value);
	rc = write(fd, cmd, len);
	close(fd);
	return rc < 0 ? -1 : 0;
}

static long count_swapped_pages(void *vaddr, long nr_pages)
{
	uint64_t entries[512];
	long count = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);
	off_t off = ((uintptr_t)vaddr / page_size) * sizeof(uint64_t);

	if (fd < 0)
		return -1;
	while (nr_pages) {
		long nr = nr_pages < 512 ? nr_pages : 512;
		ssize_t bytes = pread(fd, entries, nr * sizeof(*entries), off);
		long i;

		if (bytes != nr * sizeof(*entries)) {
			count = -1;
			break;
		}
		for (i = 0; i < nr; i++)
			count += !!(entries[i] & (1ULL << 62));
		off += bytes;
		nr_pages -= nr;
	}
	close(fd);
	return count;
}

/*
 * Fill DRAM so the balloon's migration has nowhere to relocate to.
 *
 * alloc_contig_range() clears a block by MIGRATING its residents, and while a
 * common node has room that succeeds and swaps nothing -- correct, but it means
 * ACR_FLAGS_RECLAIM never fires, because the reclaim rung is a fallback for
 * migration failing, not a mode that replaces it.  Nothing short of real
 * pressure makes migration fail, so the test creates it.
 *
 * Keep the ballast mlocked.  Otherwise reclaim can swap the ballast itself
 * and migration still finds a destination, so ACR_FLAGS_RECLAIM never needs
 * to evict the CRAM folios.  Leave @spare_mb behind for kernel and swap I/O
 * allocations.
 *
 * Return: the mapping, or NULL if DRAM could not be filled (caller SKIPs).
 */
static void *dram_ballast(long spare_mb, size_t *out_len)
{
	long free_kb = -1, len;
	char line[256];
	void *b;
	FILE *f;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return NULL;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "MemAvailable: %ld kB", &free_kb) == 1)
			break;
	fclose(f);
	if (free_kb <= 0)
		return NULL;

	len = (free_kb - spare_mb * 1024) * 1024;
	if (len <= 0)
		return NULL;

	b = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (b == MAP_FAILED)
		return NULL;
	memset(b, 0, len);
	if (mlock(b, len)) {
		munmap(b, len);
		return NULL;
	}
	*out_len = len;
	return b;
}

int main(int argc, char **argv)
{
	int cram_nid;
	long present, free0, demote_pages, len, i;
	long pswp0, pswp1, swapped = 0;
	size_t ballast_len = 0;
	void *ballast;
	unsigned char *p;
	int data_ok = 1;

	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <cram_nid> <balloon-target-path> <no-alloc-path>\n",
			argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	page_size = sysconf(_SC_PAGESIZE);

	present = node_present_pages(cram_nid);
	if (present <= 0) {
		fprintf(stderr, "cram node %d not present\n", cram_nid);
		return 2;
	}
	/* Demote ~85% of the node so a full inflate must clear residents. */
	demote_pages = present * 85 / 100;
	len = demote_pages * page_size;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(p, PATTERN, len);

	if (cram_pageout(p, len))
		perror("madvise(PAGEOUT)");

	free0 = node_vmstat(cram_nid, "nr_free_pages");
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

	/*
	 * Opt-in, because it is expensive and this harness cannot afford it.
	 *
	 * The balloon acquires blocks with alloc_contig_range(), which clears
	 * them by MIGRATING the residents.  While a common node has room that
	 * succeeds and swaps nothing -- correct behaviour, and not something to
	 * rank against eviction, but it means the reclaim rung never fires.
	 * Only real DRAM pressure makes migration fail.
	 *
	 * The mlocked fill deliberately leaves little headroom and can make the
	 * guest unresponsive, so keep it opt-in rather than imposing that cost on
	 * the normal test run.
	 */
	if (!getenv("CRAM_SWAP_FILL_DRAM")) {
		fprintf(stderr,
			"SKIP: DRAM has room; set CRAM_SWAP_FILL_DRAM=1 to force eviction\n");
		munmap(p, len);
		return 3;
	}

	ballast = dram_ballast(128, &ballast_len);
	if (!ballast) {
		fprintf(stderr, "could not fill DRAM; migration would absorb the inflate\n");
		munmap(p, len);
		return 3;
	}

	pswp0 = vmstat("pswpout");

	/*
	 * Inflate past free with rung 1 (ACR_FLAGS_RECLAIM).  DRAM is full, so
	 * migration fails and the reclaim rung swaps the residents out instead.
	 */
	{
		if (set_value(argv[3], 0) ||
		    set_value(argv[2], present)) {
			perror("inflate");
			munmap(ballast, ballast_len);
			munmap(p, len);
			return 2;
		}
		for (i = 0; i < 120; i++) {
			swapped = count_swapped_pages(p, demote_pages);
			if (swapped > 0)
				break;
			usleep(500000);
		}
	}

	pswp1 = vmstat("pswpout");
	munmap(ballast, ballast_len);

	/* How many of our pages became swap entries (left CRAM to swap)? */
	swapped = count_swapped_pages(p, demote_pages);

	/* Release the reservation and fault everything back in from swap. */
	set_value(argv[2], 0);
	set_value(argv[3], 1);
	for (i = 0; i < demote_pages; i++)
		if (p[i * page_size] != PATTERN ||
		    p[i * page_size + page_size - 1] != PATTERN)
			data_ok = 0;

	fprintf(stderr,
		"pswpout %ld->%ld (+%ld) swapped_pages=%ld data_ok=%d\n",
		pswp0, pswp1, pswp1 - pswp0, swapped, data_ok);

	munmap(p, len);

	/* Contract: physical swap writeback happened and data survived swap-in. */
	if (swapped == 0)
		return 1;	/* the test's CRAM folios did not reach swap */
	if (!data_ok)
		return 1;	/* swap-in corrupted data */
	printf("writeback-to-swap OK: +%ld pswpout, %ld pages swapped, data intact\n",
	       pswp1 - pswp0, swapped);
	return 0;
}
