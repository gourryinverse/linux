// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM allocation-gate probe.
 *
 * MADV_PAGEOUT a private-anon region and report where it landed: when CRAM
 * accepts demotions the pages end up on the cram node; when the driver has
 * revoked allocation (cram_allow_allocation(false)) or applied transient
 * back-pressure, cram_pick_node() refuses and the pages spill to swap instead.
 *
 * Usage: cram_gate_tool <cram_nid>
 * Output: "demote_delta=<n> oncram=<pages> total=<pages>"
 * Exit:   0 region demoted onto cram (gate OPEN)
 *         3 region did NOT demote onto cram (gate CLOSED / no swap fallback)
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

#define CRAM_DBG    "/sys/kernel/debug/cram"
#define DEMOTE_COUNT CRAM_DBG "/demote_count"
#define PATTERN  0xAB
#define NR_PAGES 4096		/* 16M region -- comfortably exceeds CHUNK */

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

/* pages of mapping @v currently resident on NUMA node @nid (via numa_maps). */
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
	long len, before, after, oncram;
	unsigned char *p;
	int nid;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <cram_nid>\n", argv[0]);
		return 2;
	}
	nid = atoi(argv[1]);
	page_size = sysconf(_SC_PAGESIZE);
	len = (long)NR_PAGES * page_size;

	before = read_long(DEMOTE_COUNT);

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(p, PATTERN, len);
	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");

	/* read-fault back so numa_maps reflects the post-reclaim location */
	for (long i = 0; i < len; i += page_size)
		(void)((volatile unsigned char *)p)[i];

	after = read_long(DEMOTE_COUNT);
	oncram = on_node(p, nid);
	printf("demote_delta=%ld oncram=%ld total=%ld\n",
	       (before >= 0 && after >= 0) ? after - before : -1,
	       oncram, (long)NR_PAGES);

	munmap(p, len);
	return oncram > NR_PAGES / 2 ? 0 : 3;
}
