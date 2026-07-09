// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM-fault vs swap-fault latency benchmark.
 *
 * Measures, head to head on the same machine, the per-page fault latency of:
 *
 *   cram-fault: an anon page demoted onto the CRAM node (present, read-only).
 *               The timed event is the FIRST WRITE to it, which triggers cram
 *               promotion (migrate cram->DRAM).  (A read of a cram folio is
 *               zero-copy/in-place and ~free; the cost that matters is the
 *               write-promote.  The first READ is also measured as a separate,
 *               clearly-labelled cell so the report can show it is ~free.)
 *   swap-fault: an anon page swapped out to the physical swap device (NOT on
 *               cram).  The timed event is the first access fault that swaps
 *               it back in.
 *
 * Isolation (same machine, same run): both phases arm pages with the SAME
 * MADV_PAGEOUT.  Where MADV_PAGEOUT lands depends on the global demotion knob
 * /sys/kernel/mm/numa/demotion_enabled:
 *   - knob = 1  -> reclaim demotes the anon folio onto the cram node (verified
 *                  present + on_node(cram)); this arms a cram-fault.
 *   - knob = 0  -> no demotion target, so reclaim swaps the anon folio straight
 *                  to the physical swap device (verified via the pagemap swap
 *                  bit, off cram); this arms a PURE swap-fault that never
 *                  transited cram.
 * The tool flips the knob per phase and restores it on exit.  This is the clean
 * "pure swap, never cram" baseline asked for; the .sh enables demotion before
 * launching, and the tool owns the knob for the swap phase.
 *
 * Each measured page is faulted exactly once (a fault is a one-shot event): a
 * batch of fresh anon pages is armed (paged out), then each page's single
 * faulting access is bracketed by a high-resolution clock.  Batches are re-armed
 * until >= samples faults are collected per cell.  Access order is sequential or
 * a precomputed random permutation (the permutation is built once; the access
 * itself is O(1) so RNG cost never pollutes a timing bracket).
 *
 * Usage: cram_perf_tool <cram_nid> [samples] [mono|tsc]
 * Exit: 0 success, 2 usage/setup error, 3 environmental (could not establish
 *       cram demotion or swap).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

#define DEMOTE_COUNT  "/sys/kernel/debug/cram/demote_count"
#define DEMOTION_KNOB "/sys/kernel/mm/numa/demotion_enabled"
#define CRAM_CONTROL  "/sys/kernel/debug/cram/control"
#define PATTERN       0xAB
#define NEWVAL        0xCD
#define DEF_SAMPLES   4096
#define BATCH_PAGES   512	/* pages armed (paged out) per batch */
#define WARMUP_BATCH  1	/* discard the first batch's faults */

static long page_size;
static int cram_nid;
enum clkmode { CLK_MONO, CLK_TSC };
static enum clkmode clkmode = CLK_MONO;
static double tsc_ghz;		/* measured TSC cycles per ns, when CLK_TSC */

/* ----- sysfs / pagemap helpers (mirroring cram_ops_tool.c) ----- */

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

static int write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	int rc;

	if (fd < 0)
		return -1;
	rc = write(fd, val, strlen(val));
	close(fd);
	return rc < 0 ? -1 : 0;
}

/* CRAM demotion is NOT gated by numa/demotion_enabled; block it on the cram
 * node so MADV_PAGEOUT of anon goes straight to swap (the pure swap baseline). */
static int cram_ctl(const char *cmd, int nid)
{
	char b[32];

	snprintf(b, sizeof(b), "%s %d 0", cmd, nid);
	return write_str(CRAM_CONTROL, b);
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
static int present(void *v) { return !!(pagemap(v) & (1ULL << 63)); }
static int swapped(void *v) { return !!(pagemap(v) & (1ULL << 62)); }

static long on_node(void *v, int nid)
{
	char line[4096], key[32], tok[32];
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

/* ----- timing ----- */

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
static inline uint64_t rdtsc_serialized(void)
{
	unsigned aux;
	__builtin_ia32_lfence();
	return __rdtscp(&aux);		/* rdtscp; ordered after prior insns */
}
static inline uint64_t rdtsc_pre(void)
{
	__builtin_ia32_lfence();
	uint64_t t = __rdtsc();
	__builtin_ia32_lfence();
	return t;
}
#else
static inline uint64_t rdtsc_serialized(void) { return 0; }
static inline uint64_t rdtsc_pre(void) { return 0; }
#endif

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Calibrate TSC against CLOCK_MONOTONIC; returns cycles/ns. 0 if unusable. */
static double calibrate_tsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
	uint64_t c0, c1, n0, n1;

	n0 = now_ns();
	c0 = rdtsc_serialized();
	/* busy ~50ms */
	while (now_ns() - n0 < 50000000ULL)
		;
	c1 = rdtsc_serialized();
	n1 = now_ns();
	if (n1 <= n0)
		return 0;
	return (double)(c1 - c0) / (double)(n1 - n0);
#else
	return 0;
#endif
}

/* Bracket one faulting byte access (read or write) and return latency in ns. */
static uint64_t time_fault(volatile unsigned char *addr, int do_write)
{
	if (clkmode == CLK_TSC && tsc_ghz > 0) {
		uint64_t a = rdtsc_pre(), b;

		if (do_write)
			*addr = NEWVAL;
		else
			(void)*addr;
		b = rdtsc_serialized();
		return (uint64_t)((double)(b - a) / tsc_ghz);
	} else {
		uint64_t a = now_ns(), b;

		if (do_write)
			*addr = NEWVAL;
		else
			(void)*addr;
		b = now_ns();
		return b - a;
	}
}

/* ----- stats ----- */

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static uint64_t pct(uint64_t *s, long n, double p)
{
	long idx = (long)((p * (double)n) + 0.9999999) - 1;	/* ceil(p*n)-1 */

	if (idx < 0)
		idx = 0;
	if (idx >= n)
		idx = n - 1;
	return s[idx];
}

struct cell {
	const char *fault;	/* "cram-fault" / "swap-fault" */
	const char *order;	/* "sequential" / "random" */
	uint64_t mean, p50, p90, p99, min, max;
	long n;
};

static void summarize(struct cell *c, uint64_t *s, long n)
{
	long i;
	double sum = 0;

	qsort(s, n, sizeof(*s), cmp_u64);
	for (i = 0; i < n; i++)
		sum += (double)s[i];
	c->n = n;
	c->mean = n ? (uint64_t)(sum / (double)n) : 0;
	c->min = s[0];
	c->max = s[n - 1];
	c->p50 = pct(s, n, 0.50);
	c->p90 = pct(s, n, 0.90);
	c->p99 = pct(s, n, 0.99);
}

/* Tiny self-contained PRNG so timing brackets never call into libc rand state. */
static unsigned long rng_state = 0x9e3779b97f4a7c15UL;
static unsigned long rng_next(void)
{
	/* splitmix64 */
	unsigned long z = (rng_state += 0x9e3779b97f4a7c15UL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9UL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebUL;
	return z ^ (z >> 31);
}

/* ----- arming + measurement ----- */

/*
 * Arm a fresh batch: mmap @pages anon, dirty them, MADV_PAGEOUT.  In cram mode
 * the folios demote onto the cram node (present, RO); in swap mode they go to
 * the physical swap device.  Returns the region (caller munmaps) or NULL on a
 * batch that did not arm.  @armed gets the count of pages that genuinely armed
 * (present-on-cram, or swapped-out) starting from page 0.
 */
static unsigned char *arm_batch(long pages, int cram, long *armed)
{
	long len = pages * page_size, i, ok = 0;
	unsigned char *p;
	long d0 = 0, d1 = 0;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	memset(p, PATTERN, len);
	if (cram)
		d0 = read_long(DEMOTE_COUNT);
	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");
	if (cram) {
		d1 = read_long(DEMOTE_COUNT);
		if (d1 <= d0) {		/* nothing demoted at all */
			munmap(p, len);
			return NULL;
		}
	}
	/* Count contiguous armed pages from the front (those we will measure). */
	for (i = 0; i < pages; i++) {
		unsigned char *a = p + i * page_size;

		if (cram) {
			if (present(a) && on_node(a, cram_nid) > 0)
				ok++;
			else
				break;
		} else {
			if (swapped(a) && !present(a))
				ok++;
			else
				break;
		}
	}
	*armed = ok;
	if (ok == 0) {
		munmap(p, len);
		return NULL;
	}
	return p;
}

/*
 * Run one cell: collect >= samples one-shot faults, re-arming fresh batches as
 * needed.  @cram selects cram-fault vs swap-fault; @random selects access order;
 * @do_write selects write-fault (cram promote / swap-in via write) vs read.
 * Returns 0 on success, 3 if it could never arm a batch.
 */
static int run_cell(struct cell *c, long samples, int cram, int random,
		    int do_write)
{
	size_t latsz = sizeof(uint64_t) * samples;
	uint64_t *lat;
	int *perm;
	long got = 0, batch = 0;
	int rc = 3;

	lat = malloc(latsz);
	if (!lat)
		return 2;
	perm = malloc(sizeof(int) * BATCH_PAGES);
	if (!perm) {
		free(lat);
		return 2;
	}
	/*
	 * mlock the timing buffer so a minor fault on it never lands inside a
	 * measurement bracket.  Route the pointer through a volatile sink: gcc
	 * 11's malloc/__THROW analysis otherwise emits a bogus
	 * -Wmaybe-uninitialized here even though lat is checked non-NULL above.
	 */
	{
		void * volatile latp = lat;

		if (mlock((void *)latp, latsz))
			perror("mlock(timing buffer)");	/* non-fatal */
	}

	while (got < samples) {
		long armed = 0, i;
		unsigned char *p = arm_batch(BATCH_PAGES, cram, &armed);

		if (!p) {
			/* One stray failure is fine; persistent failure => env. */
			if (++batch > 8 && got == 0)
				break;
			continue;
		}
		rc = 0;

		/* Build the access order over the [0, armed) armed pages. */
		for (i = 0; i < armed; i++)
			perm[i] = i;
		if (random) {
			for (i = armed - 1; i > 0; i--) {	/* Fisher-Yates */
				long j = (long)(rng_next() % (unsigned long)(i + 1));
				int t = perm[i];

				perm[i] = perm[j];
				perm[j] = t;
			}
		}

		for (i = 0; i < armed && got < samples; i++) {
			unsigned char *a = p + (long)perm[i] * page_size;
			uint64_t ns;

			/* Confirm this is a real one-shot fault, not a warm hit. */
			if (cram) {
				if (!present(a) || on_node(a, cram_nid) <= 0)
					continue;
			} else {
				if (present(a) || !swapped(a))
					continue;
			}
			ns = time_fault(a, do_write);

			if (batch >= WARMUP_BATCH)	/* discard warmup batch */
				lat[got++] = ns;
		}
		munmap(p, BATCH_PAGES * page_size);
		batch++;
	}

	if (got == 0) {
		free(lat);
		free(perm);
		return rc ? rc : 3;
	}
	summarize(c, lat, got);
	c->fault = cram ? "cram-fault" : "swap-fault";
	c->order = random ? "random" : "sequential";
	free(lat);
	free(perm);
	return 0;
}

static void emit(const struct cell *c)
{
	if (!c->n)
		return;
	printf("RESULT %s %s mean=%" PRIu64 " p50=%" PRIu64 " p90=%" PRIu64
	       " p99=%" PRIu64 " min=%" PRIu64 " max=%" PRIu64 " n=%ld\n",
	       c->fault, c->order, c->mean, c->p50, c->p90, c->p99,
	       c->min, c->max, c->n);
}

static void table_row(const struct cell *c)
{
	if (!c->n)
		return;
	printf("%-11s %-11s %9" PRIu64 " %9" PRIu64 " %9" PRIu64 " %9" PRIu64
	       " %9" PRIu64 " %9" PRIu64 " %7ld\n",
	       c->fault, c->order, c->mean, c->p50, c->p90, c->p99,
	       c->min, c->max, c->n);
}

int main(int argc, char **argv)
{
	long samples = DEF_SAMPLES;
	struct cell cram_seq = {0}, cram_rnd = {0}, swap_seq = {0}, swap_rnd = {0};
	struct cell cram_read = {0};
	cpu_set_t set;
	char saved_knob[8] = "1";
	long sv;
	int rc, ret = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cram_nid> [samples] [mono|tsc]\n",
			argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	page_size = sysconf(_SC_PAGESIZE);
	if (argc >= 3 && atol(argv[2]) > 0)
		samples = atol(argv[2]);
	if (argc >= 4 && !strcmp(argv[3], "tsc"))
		clkmode = CLK_TSC;

	/* Pin to one CPU to cut scheduler noise from the timing brackets. */
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		perror("sched_setaffinity");

	if (clkmode == CLK_TSC) {
		tsc_ghz = calibrate_tsc();
		if (tsc_ghz <= 0) {
			fprintf(stderr, "tsc calibration failed; using CLOCK_MONOTONIC\n");
			clkmode = CLK_MONO;
		} else {
			fprintf(stderr, "tsc calibrated: %.4f cycles/ns\n", tsc_ghz);
		}
	}
	fprintf(stderr, "clock=%s samples=%ld batch=%d cram_nid=%d\n",
		clkmode == CLK_TSC ? "tsc" : "mono", samples, BATCH_PAGES,
		cram_nid);

	/* Save the demotion knob; we own it for the duration. */
	sv = read_long(DEMOTION_KNOB);
	if (sv == 0)
		strcpy(saved_knob, "0");

	/* --- CRAM phase: demotion ON, MADV_PAGEOUT lands on the cram node. --- */
	if (write_str(DEMOTION_KNOB, "1"))
		fprintf(stderr, "warn: could not set demotion_enabled=1\n");
	rc = run_cell(&cram_seq, samples, /*cram=*/1, /*random=*/0, /*write=*/1);
	if (rc == 3) {
		fprintf(stderr, "env: could not arm any cram-fault batch\n");
		ret = 3;
		goto restore;
	} else if (rc) {
		ret = rc;
		goto restore;
	}
	run_cell(&cram_rnd, samples, 1, 1, 1);
	/* Optional: first-READ of a cram folio (expected ~free, zero-copy). */
	run_cell(&cram_read, samples, 1, 0, 0);
	cram_read.fault = "cram-read";

	/* --- SWAP phase: block cram demotion so MADV_PAGEOUT goes to swap. --- */
	if (cram_ctl("block", cram_nid))
		fprintf(stderr, "warn: could not block cram demotion\n");
	rc = run_cell(&swap_seq, samples, /*cram=*/0, /*random=*/0, /*write=*/0);
	if (rc == 3) {
		fprintf(stderr, "env: could not arm any swap-fault batch (swap on?)\n");
		cram_ctl("unblock", cram_nid);
		ret = 3;
		goto restore;
	} else if (rc) {
		cram_ctl("unblock", cram_nid);
		ret = rc;
		goto restore;
	}
	run_cell(&swap_rnd, samples, 0, 1, 0);
	cram_ctl("unblock", cram_nid);

restore:
	write_str(DEMOTION_KNOB, saved_knob);

	if (ret == 3 || (cram_seq.n == 0 && swap_seq.n == 0))
		return ret ? ret : 3;

	/* Machine-parseable block for the orchestrator to scrape. */
	emit(&cram_seq);
	emit(&cram_rnd);
	emit(&swap_seq);
	emit(&swap_rnd);
	emit(&cram_read);

	/* Human table. */
	printf("\n");
	printf("%-11s %-11s %9s %9s %9s %9s %9s %9s %7s   (ns)\n",
	       "fault", "order", "mean", "p50", "p90", "p99", "min", "max", "n");
	table_row(&cram_seq);
	table_row(&cram_rnd);
	table_row(&swap_seq);
	table_row(&swap_rnd);
	table_row(&cram_read);

	return ret;
}
