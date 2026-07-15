// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM file-tier latency probe (companion to the anon-focused cram_perf_tool).
 *
 * Populates a file, demotes its clean pages onto the CRAM node via MADV_PAGEOUT
 * (reclaim's demote-in installs them present + read-only, in place), then times
 * the read-only-tier access costs the plan (section 4b.1) asks for:
 *
 *   file-read   in-place mmap read of a demoted file page (zero-copy, ~free)
 *   file-write  first write to a demoted file page (COW-promote off CRAM)
 *   file-xproc  a forked child read-faults the file the parent demoted
 *
 * The driving .sh owns provisioning (a CRAM node + demotion_enabled + a real FS
 * disk); this tool measures and prints `RESULT` lines.  Exit 0 success,
 * 2 usage/setup, 3 environmental (could not establish the demoted-on-cram state).
 *
 * Usage: cram_file_perf_tool <cram_nid> <file_path> [samples]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

static long ps;

static uint64_t pagemap_entry(void *v)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	uint64_t e = 0;
	off_t off = ((uintptr_t)v / ps) * sizeof(uint64_t);

	if (fd < 0)
		return 0;
	if (pread(fd, &e, sizeof(e), off) != sizeof(e))
		e = 0;
	close(fd);
	return e;
}
static int present(void *v) { return !!(pagemap_entry(v) & (1ULL << 63)); }

/* pages of the mapping at @v (len @len) resident on node @nid, per numa_maps */
static long on_node(void *v, size_t len, int nid)
{
	char line[512], key[32];
	FILE *f = fopen("/proc/self/numa_maps", "r");
	unsigned long start = (unsigned long)v;
	long n = 0;

	if (!f)
		return -1;
	snprintf(key, sizeof(key), "N%d=", nid);
	while (fgets(line, sizeof(line), f)) {
		unsigned long a;
		char *p;

		if (sscanf(line, "%lx", &a) != 1 || a < start || a >= start + len)
			continue;
		if ((p = strstr(line, key)))
			n += strtol(p + strlen(key), NULL, 10);
	}
	fclose(f);
	return n;
}

static inline uint64_t ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static int cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}
static void report(const char *tag, uint64_t *s, long n)
{
	uint64_t sum = 0;
	long i;

	if (n <= 0) { printf("RESULT %s samples=0\n", tag); return; }
	qsort(s, n, sizeof(*s), cmp);
	for (i = 0; i < n; i++)
		sum += s[i];
	printf("RESULT %s samples=%ld mean_ns=%"PRIu64" p50_ns=%"PRIu64" p90_ns=%"PRIu64" p99_ns=%"PRIu64"\n",
	       tag, n, sum / n, s[n * 50 / 100], s[n * 90 / 100], s[n * 99 / 100]);
}

int main(int argc, char **argv)
{
	unsigned long mb = 64, len, npages, i;
	int cram_nid, fd;
	uint64_t *lat;
	char *p;
	long oncram;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <cram_nid> <file_path> [samples_MB]\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	if (argc >= 4)
		mb = strtoul(argv[3], NULL, 0);
	ps = sysconf(_SC_PAGESIZE);
	len = mb << 20;
	npages = len / ps;

	fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, len)) { perror("file"); return 2; }
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 2; }
	lat = calloc(npages, sizeof(*lat));
	if (!lat) return 2;

	for (i = 0; i < len; i += ps)		/* populate + flush so pages are clean */
		p[i] = (char)(i >> 12);
	msync(p, len, MS_SYNC);

	if (madvise(p, len, MADV_PAGEOUT)) { perror("madvise(PAGEOUT)"); return 3; }

	oncram = on_node(p, len, cram_nid);
	printf("RESULT setup MB=%lu pages=%lu on_cram=%ld present0=%d\n",
	       mb, npages, oncram, present(p));
	if (oncram <= 0) {
		fprintf(stderr, "file did not demote to CRAM node %d\n", cram_nid);
		return 3;		/* environmental: no cram demote happened */
	}

	/* file-read: in-place read of each demoted page */
	for (i = 0; i < npages; i++) {
		volatile char c;
		uint64_t t = ns();
		c = p[i * ps];
		lat[i] = ns() - t;
		(void)c;
	}
	report("file-read", lat, npages);

	/* file-xproc: child read-faults the same demoted file */
	{
		pid_t pid = fork();
		if (pid == 0) {
			volatile char c = 0;
			for (i = 0; i < npages; i++)
				c += p[i * ps];
			_exit((int)c & 1);
		} else if (pid > 0) {
			int st;
			uint64_t t = ns();
			waitpid(pid, &st, 0);
			printf("RESULT file-xproc child_read_all_ns=%"PRIu64" pages=%lu\n",
			       ns() - t, npages);
		}
	}

	/* file-write: first write to each page -> COW-promote off cram */
	for (i = 0; i < npages; i++) {
		uint64_t t = ns();
		p[i * ps] ^= 0xff;
		lat[i] = ns() - t;
	}
	report("file-write", lat, npages);
	printf("RESULT after_write on_cram=%ld\n", on_node(p, len, cram_nid));

	munmap(p, len);
	close(fd);
	unlink(argv[2]);
	return 0;
}
