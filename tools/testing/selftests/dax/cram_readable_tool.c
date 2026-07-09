// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM read-only tier round-trip test.
 *
 * Maps anonymous memory, pages it out (MADV_PAGEOUT) so the reclaim path
 * demotes it onto the CRAM private node by migration, and verifies the
 * read-only tier contract:
 *
 *   - after pageout the folio is PRESENT (not swapped) and resident on the
 *     CRAM node  -> demote-in installed it present read-only;
 *   - reads return the original contents with no fault            -> zero-copy;
 *   - a write changes the physical frame and moves the page off the CRAM node
 *     with the new contents                                       -> COW-promote.
 *
 * Usage: cram_readable_tool <cram_nid>
 * Exit 0 only if demote happened, the page was present-on-cram and readable,
 * and the write promoted it off-cram with contents intact.
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

#define DEMOTE_COUNT "/sys/kernel/debug/cram/demote_count"
#define PATTERN 0xAB
#define NEWVAL  0xCD
#define NR_PAGES 64

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
static int page_swapped(void *vaddr) { return !!(pagemap_entry(vaddr) & (1ULL << 62)); }

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
	char line[4096], key[32], tok[32];
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

int main(int argc, char **argv)
{
	int cram_nid, rc = 0;
	long len, demote0, demote1, nb, na;
	uint64_t pfn_before, pfn_after;
	int present, swapped, read_ok, write_ok;
	unsigned char *p;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cram_nid>\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	page_size = sysconf(_SC_PAGESIZE);
	len = (long)NR_PAGES * page_size;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(p, PATTERN, len);

	demote0 = read_long(DEMOTE_COUNT);
	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");
	demote1 = read_long(DEMOTE_COUNT);

	/* Read-only tier state: present (not swapped), on the cram node. */
	present = page_present(p);
	swapped = page_swapped(p);
	nb = pages_on_node(p, cram_nid);
	pfn_before = page_pfn(p);
	read_ok = (p[0] == PATTERN) && (p[len - 1] == PATTERN);

	fprintf(stderr,
		"diag: demote %ld->%ld present=%d swapped=%d on_cram=%ld pfn=0x%" PRIx64 " read_ok=%d\n",
		demote0, demote1, present, swapped, nb, pfn_before, read_ok);

	if (demote1 <= demote0 || nb <= 0) {
		printf("page did not demote to CRAM (demote %ld->%ld on_cram=%ld)\n",
		       demote0, demote1, nb);
		return 3;	/* environmental */
	}

	/* Write -> COW-promote off the device. */
	p[0] = NEWVAL;
	pfn_after = page_pfn(p);
	na = pages_on_node(p, cram_nid);
	write_ok = (p[0] == NEWVAL);

	printf("on_cram %ld->%ld pfn 0x%" PRIx64 "->0x%" PRIx64 " present=%d swapped=%d read_ok=%d write_ok=%d\n",
	       nb, na, pfn_before, pfn_after, present, swapped, read_ok, write_ok);

	/* Contract checks. */
	if (!present || swapped)	rc |= 1;	/* must be present, not swapped */
	if (!read_ok)			rc |= 2;	/* zero-copy read intact */
	if (!write_ok)			rc |= 4;	/* write took effect */
	if (!pfn_after || pfn_after == pfn_before)
					rc |= 8;	/* must have promoted (new frame) */
	if (na >= nb)			rc |= 16;	/* must have left the cram node */

	munmap(p, len);
	return rc ? 1 : 0;
}
