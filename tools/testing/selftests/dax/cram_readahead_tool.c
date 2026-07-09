// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM resident file tier -- readahead / sequential-read serves in place.
 *
 * A resident CRAM file folio must be served to a buffered read IN PLACE (a pure
 * read needs no write fence and does not pin), not promoted off the tier or
 * dropped-and-re-read from disk.  Verifies:
 *   R1 a sequential read(2) over a CRAM-resident file leaves the folios resident
 *      (pfn unchanged, still on-cram) and returns correct bytes -> served in
 *      place, readahead did not evict/re-read them;
 *   R2 splice() (which parks folios in a pipe -- a long-term pin) DOES promote
 *      the folios off the tier, upholding the "never long-term pinned" invariant.
 *
 * Usage: cram_readahead_tool <cram_nid> <mnt_dir>
 * Exit 0 clean, 1 violation, 3 environmental, 2 setup error.
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
#ifndef MADV_RANDOM
#define MADV_RANDOM 1
#endif

#define PATTERN 0xAB
#define NR_PAGES 64
#define PROBE 7

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

static int make_file(const char *path, unsigned char val)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	unsigned char *buf;
	long i;

	if (fd < 0)
		return -1;
	buf = malloc((size_t)NR_PAGES * page_size);
	if (!buf) {
		close(fd);
		return -1;
	}
	memset(buf, val, (size_t)NR_PAGES * page_size);
	for (i = 0; i < NR_PAGES; i++)
		if (pwrite(fd, buf + i * page_size, page_size,
			   i * page_size) != page_size) {
			free(buf);
			close(fd);
			return -1;
		}
	free(buf);
	fsync(fd);
	return fd;
}

/* Observation mmap: fault RO + MADV_PAGEOUT -> demote clean folios onto CRAM. */
static unsigned char *demote(int fd, long *lenp, long *resident)
{
	long len = (long)NR_PAGES * page_size, i;
	unsigned char *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	volatile unsigned char sink = 0;

	if (p == MAP_FAILED)
		return NULL;
	madvise(p, len, MADV_RANDOM);
	for (i = 0; i < NR_PAGES; i++)
		sink += p[i * page_size];
	(void)sink;
	if (madvise(p, len, MADV_PAGEOUT)) {
		munmap(p, len);
		return NULL;
	}
	sink += p[PROBE * page_size];
	*lenp = len;
	*resident = pages_on_node(p, cram_nid);
	return p;
}

int main(int argc, char **argv)
{
	const char *mnt;
	char path[256];
	long len, resident, oncram_before, oncram_after;
	unsigned char *p;
	uint64_t pfn_before, pfn_after;
	unsigned long probe_off;
	int fd, rfd, rc = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <cram_nid> <mnt_dir>\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	mnt = argv[2];
	page_size = sysconf(_SC_PAGESIZE);
	probe_off = (unsigned long)PROBE * page_size;

	snprintf(path, sizeof(path), "%s/cram_ra", mnt);
	fd = make_file(path, PATTERN);
	if (fd < 0) { fprintf(stderr, "make_file\n"); return 2; }
	p = demote(fd, &len, &resident);		/* observation mapping */
	if (!p) { close(fd); return 2; }
	if (resident <= 0) { munmap(p, len); close(fd); return 3; }

	/* ---- R1: sequential read(2) serves resident folios in place ---- */
	oncram_before = pages_on_node(p, cram_nid);
	pfn_before = page_pfn(&p[probe_off]);
	{
		unsigned char *buf = malloc(len);
		ssize_t n, tot = 0;
		int ok = 1;

		rfd = open(path, O_RDONLY);		/* fresh fd -> readahead active */
		if (rfd < 0 || !buf) { fprintf(stderr, "open/buf\n"); free(buf); return 2; }
		while (tot < len && (n = pread(rfd, buf + tot, len - tot, tot)) > 0)
			tot += n;
		close(rfd);
		for (long i = 0; i < len; i++)
			if (buf[i] != PATTERN) { ok = 0; break; }
		free(buf);
		if (!ok || tot != len) { fprintf(stderr, "R1 bad read bytes\n"); rc = 1; }
	}
	pfn_after = page_pfn(&p[probe_off]);
	oncram_after = pages_on_node(p, cram_nid);
	if (pfn_after != pfn_before || oncram_after < oncram_before) {
		fprintf(stderr, "R1 sequential read PROMOTED/dropped resident folios "
			"(pfn %llx->%llx oncram %ld->%ld) -- not served in place\n",
			(unsigned long long)pfn_before, (unsigned long long)pfn_after,
			oncram_before, oncram_after);
		rc = 1;
	}
	printf("R1 read-in-place: oncram %ld->%ld pfn %s %s\n",
	       oncram_before, oncram_after,
	       pfn_after == pfn_before ? "kept" : "MOVED", rc ? "FAIL" : "ok");

	/* ---- R2: splice() promotes off the tier (long-term pin) ---- */
	{
		int pfd[2];
		int f = 0;

		oncram_before = pages_on_node(p, cram_nid);
		rfd = open(path, O_RDONLY);
		if (rfd >= 0 && pipe(pfd) == 0) {
			ssize_t s = splice(rfd, NULL, pfd[1], NULL, page_size, 0);
			unsigned char drain[8192];

			if (s > 0)
				(void)!read(pfd[0], drain, s < (ssize_t)sizeof(drain) ?
					    s : (ssize_t)sizeof(drain));
			close(pfd[0]); close(pfd[1]);
		}
		if (rfd >= 0)
			close(rfd);
		oncram_after = pages_on_node(p, cram_nid);
		if (oncram_after >= oncram_before) {
			fprintf(stderr, "R2 splice did NOT promote off CRAM "
				"(oncram %ld->%ld) -- pipe pin left on the tier\n",
				oncram_before, oncram_after);
			f = 1;
		}
		printf("R2 splice-promotes: oncram %ld->%ld %s\n",
		       oncram_before, oncram_after, f ? "FAIL" : "ok");
		rc |= f;
	}

	munmap(p, len);
	close(fd);
	return rc;
}
