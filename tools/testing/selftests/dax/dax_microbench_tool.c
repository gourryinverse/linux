// SPDX-License-Identifier: GPL-2.0
/*
 * Tiering microbenchmark for the dax / private-node / cram test matrix.
 *
 * Drives a hot working set larger than the top tier so the kernel must demote
 * to the lower tier (a dax/kmem node, cram, or swap) and reports the OS
 * overhead of doing so: access throughput, per-access latency percentiles, and
 * the demotion / swap / refault vmstat deltas across the run.
 *
 * The driving .sh owns provisioning: it binds the workload to the top tier
 * (cpuset.mems / mbind) and sizes @total_MB above that tier's capacity so the
 * access loop forces demotion.  This tool just runs the loop and reports facts
 * on stdout as `RESULT key=value` lines, exiting 0 (KSFT_SKIP=4 on setup error).
 *
 *   anon <ro|rw> <total_MB> <secs> <seq|rand>
 *   file <ro|rw> <total_MB> <secs> <seq|rand> <path>
 *
 * ro = read each page (readable working set), rw = write each page (writable).
 * seq = sequential sweep, rand = uniform random page pick.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>

#define KSFT_SKIP 4

/* vmstat counters snapshotted around the run and reported as deltas. */
static const char * const kCounters[] = {
	"pgdemote_kswapd", "pgdemote_direct", "pgdemote_khugepaged",
	"pgdemote_swap_fallback", "pswpin", "pswpout", "pgpromote_success",
	"workingset_refault_anon", "workingset_refault_file",
	"pgscan_kswapd", "pgscan_direct",
};
#define NCOUNTERS (sizeof(kCounters) / sizeof(kCounters[0]))

static void read_vmstat(unsigned long long *out)
{
	FILE *f = fopen("/proc/vmstat", "r");
	char key[64];
	unsigned long long val;
	size_t i;

	for (i = 0; i < NCOUNTERS; i++)
		out[i] = 0;
	if (!f)
		return;
	while (fscanf(f, "%63s %llu", key, &val) == 2)
		for (i = 0; i < NCOUNTERS; i++)
			if (!strcmp(key, kCounters[i]))
				out[i] = val;
	fclose(f);
}

static unsigned int rng = 2463534242u;
static inline unsigned int xs(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
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
	unsigned long long x = *(const unsigned long long *)a;
	unsigned long long y = *(const unsigned long long *)b;

	return (x > y) - (x < y);
}

/* time 1-in-SAMPLE_EVERY accesses into a capped buffer for percentiles */
#define SAMPLE_EVERY	256
#define SAMPLE_CAP	(1u << 20)
static unsigned long long *samples;
static unsigned long nsamples;

int main(int argc, char **argv)
{
	const char *mode, *rw, *pat, *path = NULL;
	unsigned long long total, npages, secs, i, touches = 0, acc = 0;
	unsigned long long t0, tend, elapsed_ns, sctr = 0;
	unsigned long long before[NCOUNTERS], after[NCOUNTERS];
	long ps = sysconf(_SC_PAGESIZE);
	int is_file, is_rw, is_rand, fd = -1;
	char *p;
	size_t k;

	if (argc < 6) {
		fprintf(stderr, "usage: %s anon|file ro|rw <total_MB> <secs> seq|rand [path]\n",
			argv[0]);
		return KSFT_SKIP;
	}
	mode = argv[1]; rw = argv[2];
	total = strtoull(argv[3], NULL, 0) << 20;
	secs = strtoull(argv[4], NULL, 0);
	pat = argv[5];
	is_file = !strcmp(mode, "file");
	is_rw = !strcmp(rw, "rw");
	is_rand = !strcmp(pat, "rand");
	if (is_file) {
		if (argc < 7) {
			fprintf(stderr, "file mode needs <path>\n");
			return KSFT_SKIP;
		}
		path = argv[6];
	}
	if (!total || !secs) {
		fprintf(stderr, "bad size/secs\n");
		return KSFT_SKIP;
	}
	npages = total / ps;

	if (is_file) {
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (fd < 0 || ftruncate(fd, total)) {
			perror("file setup");
			return KSFT_SKIP;
		}
		p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	} else {
		p = mmap(NULL, total, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}
	if (p == MAP_FAILED) {
		perror("mmap");
		return KSFT_SKIP;
	}

	samples = calloc(SAMPLE_CAP, sizeof(*samples));
	if (!samples) {
		fprintf(stderr, "no mem for samples\n");
		return KSFT_SKIP;
	}

	/* prefault the whole region once so steady-state (not first-touch) is measured */
	for (i = 0; i < total; i += ps)
		p[i] = (char)i;

	read_vmstat(before);
	t0 = ns_now();
	tend = t0 + secs * 1000000000ull;

	for (;;) {
		unsigned long long idx = is_rand ? (xs() % npages) : (acc % npages);
		char *addr = p + idx * ps;
		int measure = (++sctr % SAMPLE_EVERY) == 0 && nsamples < SAMPLE_CAP;
		unsigned long long s = 0;

		if (measure)
			s = ns_now();
		if (is_rw)
			(*addr)++;
		else
			acc += (unsigned char)*addr;
		if (measure)
			samples[nsamples++] = ns_now() - s;

		touches++;
		if ((touches & 0xffff) == 0 && ns_now() >= tend)
			break;
	}

	tend = ns_now();
	read_vmstat(after);
	elapsed_ns = tend - t0;

	/* throughput */
	{
		double sec = elapsed_ns / 1e9;
		double tps = touches / sec;

		printf("RESULT mode=%s rw=%s pattern=%s total_MB=%llu secs=%.1f\n",
		       mode, rw, pat, total >> 20, sec);
		printf("RESULT touches=%llu touches_per_s=%.0f MBps=%.1f\n",
		       touches, tps, tps * ps / 1e6);
	}

	/* latency percentiles (ns) */
	if (nsamples) {
		qsort(samples, nsamples, sizeof(*samples), cmp_ull);
		printf("RESULT lat_samples=%lu p50_ns=%llu p90_ns=%llu p99_ns=%llu max_ns=%llu\n",
		       nsamples,
		       samples[nsamples * 50 / 100],
		       samples[nsamples * 90 / 100],
		       samples[nsamples * 99 / 100],
		       samples[nsamples - 1]);
	}

	/* vmstat deltas */
	for (k = 0; k < NCOUNTERS; k++)
		printf("RESULT %s=%llu\n", kCounters[k], after[k] - before[k]);

	if (is_file && fd >= 0) {
		close(fd);
		unlink(path);
	}
	return 0;
}
