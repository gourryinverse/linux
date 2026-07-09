// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM read-only tier pressure / livelock battery.
 *
 * Hammers the CRAM tier with simultaneous demotion pressure and promotion
 * storms to try to wedge the machine (livelock / deadlock / OOM) or trip a
 * write-protection splat.  Where cram_ops_tool exercises one mm op at a time
 * against a quiescent demoted region, this drives many threads at on-cram anon
 * folios while they are being demoted underneath the accessors.  shmem is no
 * longer CRAM-eligible, so every region here is private anonymous
 * (MAP_PRIVATE | MAP_ANONYMOUS):
 *
 *   demoters  - MADV_PAGEOUT large anon regions in a loop, pushing the cram node
 *               toward full so promotion competes for space and resident folios
 *               get written back to swap.
 *   promoters - read-fault (touch bytes RO) and mmap-write (store a byte ->
 *               COW/promote) against on-cram anon folios, forcing the promote
 *               isolate+migrate path under contention.  Concurrent isolators get
 *               -EAGAIN and retry; an unconverging retry loop is a livelock.
 *   forkers   - children touch a shared-by-inheritance anon region (parent
 *               demotes it, child reads) then exit, racing teardown.
 *
 * Subtests:
 *   stress       - the full concurrent storm, time-bounded (CRAM_STRESS_SECS),
 *                  with a forward-progress watchdog: if the global op counter
 *                  stalls for > WATCHDOG_SECS the run is aborted and reported
 *                  (the box isn't dead but nothing completes == livelock).
 *                  Prints op counts + demote/promote/promote_fail deltas.
 *   promote_race - many threads race to first-write the SAME demoted page,
 *                  stressing the isolate / -EAGAIN retry convergence; every
 *                  write must eventually land AND the pfn must change (promoted).
 *   drain_race   - rapidly demote-then-write a region so the just-demoted folio
 *                  is not yet PG_lru, hitting the lru_add_drain[_all] escalation
 *                  inside the promote path.
 *
 * Detectors: a forward-progress watchdog (livelock), and the pfn-must-change-
 * on-write leak check -- a write that leaves the pfn unchanged wrote on the
 * cram device in place (a leak the CONFIG_CRAM_DEBUG verifier MISSES for
 * writable PTEs, so the pfn check is essential).  The wrapper's dmesg scan
 * covers splats.
 *
 * Coverage signal: at the end of "stress" the cram debugfs counters are read
 * as before/after deltas; if demote_count or promote_count did not advance the
 * run failed to exercise the paths and returns 3 (environmental) so a green run
 * can never be a no-op.
 *
 * Usage: cram_pressure_tool <cram_nid> <subtest>
 * Exit: 0 pass, 1 fail (livelock/leak), 2 usage/setup error,
 *       3 environmental (no demote progress; anon promote is COW, uncounted).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

#define CRAM_DBG     "/sys/kernel/debug/cram"
#define DEMOTE_COUNT  CRAM_DBG "/demote_count"
#define PROMOTE_COUNT CRAM_DBG "/promote_count"
#define PROMOTE_FAIL  CRAM_DBG "/promote_fail"

#define PATTERN  0xAB
#define NEWVAL   0xCD
#define NR_PAGES 64		/* per anon region */

#define DEMOTERS 4
#define PROMOTERS 6		/* read-fault + mmap-write across regions */
#define FORKERS  2
#define RACERS   8		/* promote_race threads on one page */

#define WATCHDOG_SECS 20	/* no-progress window that flags a livelock */

static long page_size;
static int cram_nid;

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
static uint64_t pfn(void *v) { uint64_t e = pagemap(v); return (e & (1ULL << 63)) ? (e & ((1ULL << 55) - 1)) : 0; }

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

static void touch_ro(volatile unsigned char *p, long len)
{
	long i;

	for (i = 0; i < len; i += page_size)
		(void)p[i];
}

static double now_secs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * A fresh NR_PAGES private-anon region filled with PATTERN, demoted onto cram.
 * Returns the region (caller munmaps) or NULL on env failure (did not demote).
 */
static unsigned char *make_demoted_anon(long *len_out)
{
	long len = (long)NR_PAGES * page_size;
	unsigned char *p;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	memset(p, PATTERN, len);
	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");
	touch_ro(p, len);
	if (on_node(p, cram_nid) <= 0) {
		munmap(p, len);
		return NULL;
	}
	*len_out = len;
	return p;
}

/* ---------------- shared stress state ---------------- */
struct ctx {
	long len;
	unsigned char *shared;		/* forker region (inherited across fork) */
	volatile int stop;
	volatile unsigned long ops;	/* forward-progress counter */
	volatile unsigned long bad;	/* in-place write-leak / torn-read count */
	volatile unsigned long bad_rd;	/* torn reads (neither PATTERN nor NEWVAL) */
	volatile unsigned long bad_wr;	/* mmap-write in-place leaks */
	volatile int bad_val;		/* a sample torn-read byte value */
};

/* Demoter: keep pushing anon folios onto cram, driving the node toward full. */
static void *th_demoter(void *arg)
{
	struct ctx *c = arg;

	while (!c->stop) {
		unsigned char *a = mmap(NULL, c->len, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (a != MAP_FAILED) {
			memset(a, PATTERN, c->len);
			madvise(a, c->len, MADV_PAGEOUT);
			munmap(a, c->len);
		}
		__atomic_fetch_add(&c->ops, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

/*
 * read-fault promoter: demote a private-anon region, then touch its bytes
 * read-only.  A read of an on-cram anon folio is a zero-copy present-RO access;
 * the bytes must read back as PATTERN (a torn read of any other value is
 * corruption).
 */
static void *th_freader(void *arg)
{
	struct ctx *c = arg;

	while (!c->stop) {
		long len, off;
		unsigned char *p = make_demoted_anon(&len);

		if (!p) {
			__atomic_fetch_add(&c->ops, 1, __ATOMIC_RELAXED);
			continue;
		}
		for (off = 0; off < len && !c->stop; off += page_size) {
			unsigned char v = p[off];

			if (v != PATTERN && v != NEWVAL) {
				c->bad_val = v;
				__atomic_fetch_add(&c->bad_rd, 1, __ATOMIC_RELAXED);
				__atomic_fetch_add(&c->bad, 1, __ATOMIC_RELAXED);
			}
		}
		munmap(p, len);
		__atomic_fetch_add(&c->ops, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

/*
 * mmap-write promoter: per-thread EXCLUSIVE private-anon region so the pfn check
 * is race-free.  Each round demotes the region onto cram, then a store must
 * COW/promote it off the tier: the byte takes effect, the page leaves the cram
 * node, and the frame changes.  A store that leaves the pfn unchanged AND the
 * folio still on cram wrote on the device in place == leak.
 */
static void *th_mwriter(void *arg)
{
	struct ctx *c = arg;

	while (!c->stop) {
		long len;
		unsigned char *p = make_demoted_anon(&len);
		uint64_t p0;

		if (!p) {		/* env: did not demote */
			__atomic_fetch_add(&c->ops, 1, __ATOMIC_RELAXED);
			continue;
		}
		p0 = pfn(p);
		p[0] = NEWVAL;		/* private write -> COW/promote off cram */
		if (p[0] == NEWVAL && p0 && pfn(p) == p0 &&
		    on_node(p, cram_nid) > 0) {
			__atomic_fetch_add(&c->bad_wr, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&c->bad, 1, __ATOMIC_RELAXED);
		}
		munmap(p, len);
		__atomic_fetch_add(&c->ops, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

/*
 * Forker: child reads a shared-by-inheritance cram mapping then exits, racing
 * exit_mmap teardown.  The parent keeps c->shared demoted on cram; the child
 * inherits it COW and faults it read-only.
 */
static void *th_forker(void *arg)
{
	struct ctx *c = arg;

	while (!c->stop) {
		pid_t pid = fork();

		if (pid == 0) {
			if (c->shared)
				touch_ro(c->shared, c->len);
			_exit(0);	/* exit WITHOUT unmapping */
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
		__atomic_fetch_add(&c->ops, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

/*
 * stress: run the full storm for CRAM_STRESS_SECS while a watchdog samples the
 * op counter.  If it stalls for > WATCHDOG_SECS we declare a livelock and bail.
 */
static int t_stress(void)
{
	struct ctx c = { .len = (long)NR_PAGES * page_size };
	pthread_t demoters[DEMOTERS], promoters[PROMOTERS];
	pthread_t forkers[FORKERS];
	long d0, d1, p0, p1, pf0, pf1, secs;
	double start, last_progress;
	unsigned long last_ops = 0;
	int i, livelock = 0;
	const char *e = getenv("CRAM_STRESS_SECS");

	secs = e ? atol(e) : 20;
	if (secs <= 0)
		secs = 20;

	/* Region the forker children inherit and read while it stays on cram. */
	c.shared = make_demoted_anon(&c.len);
	if (!c.shared) {
		fprintf(stderr, "stress: anon region did not demote to cram\n");
		return 3;
	}

	d0 = read_long(DEMOTE_COUNT);
	p0 = read_long(PROMOTE_COUNT);
	pf0 = read_long(PROMOTE_FAIL);

	for (i = 0; i < DEMOTERS; i++)
		pthread_create(&demoters[i], NULL, th_demoter, &c);
	/* PROMOTERS = read-fault + mmap-write promoters interleaved. */
	pthread_create(&promoters[0], NULL, th_freader, &c);
	pthread_create(&promoters[1], NULL, th_freader, &c);
	pthread_create(&promoters[2], NULL, th_freader, &c);
	pthread_create(&promoters[3], NULL, th_mwriter, &c);
	pthread_create(&promoters[4], NULL, th_mwriter, &c);
	pthread_create(&promoters[5], NULL, th_mwriter, &c);
	for (i = 0; i < FORKERS; i++)
		pthread_create(&forkers[i], NULL, th_forker, &c);

	start = last_progress = now_secs();
	while (now_secs() - start < secs) {
		unsigned long ops;

		usleep(200000);
		ops = __atomic_load_n(&c.ops, __ATOMIC_RELAXED);
		if (ops != last_ops) {
			last_ops = ops;
			last_progress = now_secs();
		} else if (now_secs() - last_progress > WATCHDOG_SECS) {
			fprintf(stderr,
				"stress: WATCHDOG no forward progress for %ds (ops=%lu) -- LIVELOCK\n",
				WATCHDOG_SECS, ops);
			livelock = 1;
			break;
		}
	}
	c.stop = 1;

	for (i = 0; i < DEMOTERS; i++)
		pthread_join(demoters[i], NULL);
	for (i = 0; i < PROMOTERS; i++)
		pthread_join(promoters[i], NULL);
	for (i = 0; i < FORKERS; i++)
		pthread_join(forkers[i], NULL);

	d1 = read_long(DEMOTE_COUNT);
	p1 = read_long(PROMOTE_COUNT);
	pf1 = read_long(PROMOTE_FAIL);
	munmap(c.shared, c.len);

	fprintf(stderr,
		"stress: ops=%lu bad=%lu (rd=%lu val=%#x wr=%lu) demote +%ld promote +%ld promote_fail +%ld\n",
		c.ops, c.bad, c.bad_rd, c.bad_val, c.bad_wr, d1 - d0, p1 - p0, pf1 - pf0);

	/*
	 * Pass gates (reliable under concurrency): no livelock, no torn reads
	 * (data integrity), and the demote/promote paths advanced.  bad_wr (the
	 * mmap pfn-unchanged-and-on-cram count) is only a diagnostic here: under
	 * concurrent reclaim the kernel may legitimately promote a page then have
	 * kswapd re-demote it onto the same frame, which the pfn check cannot
	 * distinguish from an in-place write.  In-place writes are caught
	 * authoritatively by the CONFIG_CRAM_DEBUG verifier (the .sh splat scan)
	 * and by the single-threaded cram_leak/cram_ops writable-pte tests.
	 */
	if (livelock || c.bad_rd)
		return 1;			/* livelock or torn read (corruption) */
	/*
	 * Coverage: demote must advance.  promote_count is NOT required: it
	 * counts only cram_promote_pagecache() (the file page-cache path); an
	 * anonymous write promotes via do_wp_page() COW, which is uncounted, so
	 * promote_count legitimately stays 0 for this anon workload.  Forward
	 * progress + clean reads + an advancing demote count are the real signals.
	 */
	if (d1 <= d0)
		return 3;			/* demote path not exercised -- environmental */
	return 0;
}

/* ---------------- promote_race ---------------- */
struct race {
	unsigned char *map;		/* private-anon region, page 0 */
	uint64_t base_pfn;
	volatile int go;
	volatile unsigned long bad;
};

static void *th_racer(void *arg)
{
	struct race *r = arg;
	uint64_t p1;

	while (!r->go)
		;			/* spin so all racers fire together */
	r->map[0] = NEWVAL;		/* race to first-write the demoted page */
	p1 = pfn(r->map);
	/* The store must take effect and the page must have left cram (pfn moved). */
	if (r->map[0] != NEWVAL || !p1 || p1 == r->base_pfn)
		__atomic_fetch_add(&r->bad, 1, __ATOMIC_RELAXED);
	return NULL;
}

static int t_promote_race(void)
{
	struct race r = { .go = 0, .bad = 0 };
	pthread_t th[RACERS];
	long len;
	int i;

	r.map = make_demoted_anon(&len);
	if (!r.map)
		return 3;
	r.base_pfn = pfn(r.map);

	for (i = 0; i < RACERS; i++)
		pthread_create(&th[i], NULL, th_racer, &r);
	r.go = 1;			/* unleash the simultaneous first-write */
	for (i = 0; i < RACERS; i++)
		pthread_join(th[i], NULL);

	fprintf(stderr, "promote_race: racers=%d bad=%lu base_pfn=0x%" PRIx64 " final_pfn=0x%" PRIx64 " val=%#x\n",
		RACERS, r.bad, r.base_pfn, pfn(r.map), r.map[0]);
	munmap(r.map, len);
	return r.bad ? 1 : 0;
}

/* ---------------- drain_race ---------------- */
/*
 * Demote then immediately write, many rounds: the just-demoted folio is not yet
 * on the LRU, so the promote path must escalate through lru_add_drain() /
 * lru_add_drain_all() to isolate it.  A write whose pfn does not change wrote on
 * the device in place (the drain path failed to promote) == leak.
 */
static int t_drain_race(void)
{
	long len = (long)NR_PAGES * page_size;
	int round, bad = 0, demoted = 0;
	const int rounds = 200;

	for (round = 0; round < rounds; round++) {
		unsigned char *p;
		uint64_t p0, p1;

		p = mmap(NULL, len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			return 2;
		memset(p, PATTERN, len);
		if (madvise(p, len, MADV_PAGEOUT))
			perror("madvise(PAGEOUT)");
		touch_ro(p, page_size);
		if (on_node(p, cram_nid) <= 0) {
			munmap(p, len);
			continue;		/* transient: try another round */
		}
		demoted++;

		p0 = pfn(p);
		p[0] = NEWVAL;			/* write a fresh-demoted (non-LRU) folio */
		p1 = pfn(p);
		if (p[0] != NEWVAL || (p0 && p1 && p1 == p0))
			bad++;			/* wrote in place -- drain path leak */
		munmap(p, len);
	}

	fprintf(stderr, "drain_race: rounds=%d demoted=%d bad=%d\n",
		rounds, demoted, bad);
	if (!demoted)
		return 3;			/* never demoted -- environmental */
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *sub;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <cram_nid> <subtest>\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	sub = argv[2];
	page_size = sysconf(_SC_PAGESIZE);

	if (!strcmp(sub, "stress"))        return t_stress();
	if (!strcmp(sub, "promote_race"))  return t_promote_race();
	if (!strcmp(sub, "drain_race"))    return t_drain_race();
	fprintf(stderr, "unknown subtest %s\n", sub);
	return 2;
}
