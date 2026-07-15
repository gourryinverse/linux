// SPDX-License-Identifier: GPL-2.0
/*
 * Tier latency probes for the dax / private-node / cram test matrix.
 *
 * Reports raw access + fault latencies so the tiers can be characterised
 * before the throughput microbench runs.  The driving .sh owns node binding
 * (numactl) and swap/zswap setup; this tool just measures and prints
 * `RESULT key=value` lines, exiting 0 (KSFT_SKIP=4 on setup error).
 *
 *   access  <MB> <steps_M>   pointer-chase latency over a buffer the caller has
 *                            bound to a node -> DRAM vs CXL access latency
 *   refault <MB>             prefault anon, MADV_PAGEOUT it, then time each
 *                            page re-fault -> swap / zswap fault latency
 *                            (run with zswap off = swap, on = zswap)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>

#define KSFT_SKIP 4
#define LINE 64		/* cache-line stride to defeat prefetch */

static unsigned int rng = 88172645u;
static inline unsigned int xs(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}
static inline unsigned long long ns_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static int cmp_ull(const void *a, const void *b)
{
	unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
	return (x > y) - (x < y);
}
static void pct(unsigned long long *s, unsigned long n)
{
	if (!n)
		return;
	qsort(s, n, sizeof(*s), cmp_ull);
	printf("RESULT samples=%lu p50_ns=%llu p90_ns=%llu p99_ns=%llu max_ns=%llu\n",
	       n, s[n * 50 / 100], s[n * 90 / 100], s[n * 99 / 100], s[n - 1]);
}

static int do_access(unsigned long long mb, unsigned long long steps)
{
	unsigned long long bytes = mb << 20, nlines = bytes / LINE, i;
	unsigned long long off = 0, sink = 0, t0, dt;
	unsigned long long *perm;
	char *buf;

	buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return KSFT_SKIP; }
	perm = malloc(nlines * sizeof(*perm));
	if (!perm) { fprintf(stderr, "no mem for perm\n"); return KSFT_SKIP; }

	for (i = 0; i < nlines; i++)
		perm[i] = i;
	/* Sattolo: single cycle through all lines */
	for (i = nlines - 1; i > 0; i--) {
		unsigned long long j = xs() % i;
		unsigned long long tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
	}
	for (i = 0; i < nlines; i++)
		*(unsigned long long *)(buf + perm[i] * LINE) = perm[(i + 1) % nlines] * LINE;
	free(perm);

	/* warm the chain into the bound node, then measure */
	for (i = 0; i < nlines; i++)
		off = *(unsigned long long *)(buf + off);

	t0 = ns_now();
	for (i = 0; i < steps; i++)
		off = *(unsigned long long *)(buf + off);
	dt = ns_now() - t0;
	sink = off;

	printf("RESULT probe=access MB=%llu steps=%llu mean_ns=%.2f (sink=%llu)\n",
	       mb, steps, (double)dt / steps, sink);
	return 0;
}

static int do_refault(unsigned long long mb)
{
	long ps = sysconf(_SC_PAGESIZE);
	unsigned long long bytes = mb << 20, npages = bytes / ps, i, n = 0;
	unsigned long long *lat;
	volatile char sink = 0;
	char *buf;

	if (!(sysconf(_SC_PHYS_PAGES) > 0)) { /* nop */ }
	buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return KSFT_SKIP; }
	lat = malloc(npages * sizeof(*lat));
	if (!lat) { fprintf(stderr, "no mem for lat\n"); return KSFT_SKIP; }

	/*
	 * Prefault EVERY byte with realistic, non-zero, ~2:1-compressible content
	 * (first half of each page varies, second half zero).  An all-zero page is
	 * degenerate: swap/zswap special-case it, so the refault would measure
	 * zero-page handling instead of real swap-in vs zswap-in latency.
	 */
	for (i = 0; i < bytes; i++)
		buf[i] = ((i & (ps - 1)) < (unsigned long long)ps / 2)
			 ? (char)(0x9e3779b97f4a7c15ULL * (i + 1) >> 32)
			 : 0;
	if (madvise(buf, bytes, MADV_PAGEOUT)) {
		perror("madvise(PAGEOUT)");
		return KSFT_SKIP;		/* no swap -> can't evict */
	}
	/*
	 * MADV_PAGEOUT frees the frame for zswap (compressed) but leaves disk-swapped
	 * pages in the swap cache (frame not freed w/o pressure) -> re-access would be
	 * a cache hit, not a real swap-in.  Force true frame-eviction via our cgroup's
	 * memory.reclaim (swap-cache pages count against memory.current, so reclaiming
	 * that many bytes must free them).  Requires being run inside a cgroup2 memcg.
	 */
	{
		FILE *cf = fopen("/proc/self/cgroup", "r");
		char line[256];

		if (cf) {
			if (fgets(line, sizeof(line), cf)) {
				char *p = strstr(line, "::");   /* "0::/lat\n" */
				char *nl;

				if (p && (p += 2) && *p == '/') {
					char path[512], nbuf[64];
					int rfd;

					nl = strchr(p, '\n'); if (nl) *nl = 0;
					snprintf(path, sizeof(path),
						 "/sys/fs/cgroup%s/memory.reclaim",
						 strcmp(p, "/") ? p : "");
					rfd = open(path, O_WRONLY);
					if (rfd >= 0) {
						int k = snprintf(nbuf, sizeof(nbuf), "%llu\n", bytes);
						if (write(rfd, nbuf, k) < 0) { /* best-effort */ }
						close(rfd);
					}
				}
			}
			fclose(cf);
		}
	}
	/*
	 * Self-validating timing: consult /proc/self/pagemap per page and time ONLY
	 * pages that are actually swapped out (bit62), skipping any still resident
	 * (bit63 = a swap-cache hit, NOT a real fault).  Reports the confirmed-real
	 * fraction so a bad measurement (cache hits) is visible instead of silent.
	 * Pair with page-cluster=0 so each fault is a single real page-in (no
	 * readahead pulling neighbours back in and inflating the "cached" count).
	 */
	{
		int pmfd = open("/proc/self/pagemap", O_RDONLY);
		unsigned long long cached = 0, absent = 0;

		if (pmfd < 0) { perror("open pagemap"); return KSFT_SKIP; }
		for (i = 0; i < npages; i++) {
			unsigned long long ent = 0, t0;
			off_t off = ((unsigned long long)(uintptr_t)(buf + i * ps) / ps)
				    * (off_t)sizeof(ent);

			if (pread(pmfd, &ent, sizeof(ent), off) != (ssize_t)sizeof(ent))
				continue;
			if (ent & (1ULL << 63)) {	/* present -> swap-cache hit */
				cached++; sink += buf[i * ps]; continue;
			}
			if (!(ent & (1ULL << 62))) { absent++; continue; }  /* not swapped */
			t0 = ns_now();
			sink += buf[i * ps];		/* real swap-in / zswap-in */
			lat[n++] = ns_now() - t0;
		}
		close(pmfd);
		printf("RESULT probe=refault MB=%llu real_faults=%llu cached_hits=%llu absent=%llu real_pct=%.1f (sink=%d)\n",
		       mb, n, cached, absent, npages ? 100.0 * n / npages : 0.0, sink);
	}
	pct(lat, n);
	free(lat);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "access"))
		return do_access(strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0) * 1000000ull);
	if (argc >= 3 && !strcmp(argv[1], "refault"))
		return do_refault(strtoull(argv[2], NULL, 0));
	fprintf(stderr, "usage: %s access <MB> <steps_M> | refault <MB>\n", argv[0]);
	return KSFT_SKIP;
}
