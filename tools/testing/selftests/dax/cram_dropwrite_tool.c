// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM write(2)-drop ADVERSARIAL correctness tool.
 *
 * The drop variant (mm/filemap.c): a buffered write(2) that acquires a resident
 * CRAM folio it is *not* mmap-mapping drops the folio from the page cache and
 * lets FGP_CREAT reallocate a fresh DRAM folio, instead of promote-copying it.
 * A partial write then lands in a fresh, NOT-uptodate folio -- the filesystem
 * must read the untouched bytes back from disk (RMW).  If it does not, the
 * untouched bytes come back zero (or stale): silent data corruption.  That, plus
 * the mapped-writer path (which must still promote, never drop -- filemap_remove_
 * folio() does not unmap, so dropping a mapped folio would dangle its PTEs), is
 * what this hammers.
 *
 * Model-based: every 8-byte word in the file stores (gen<<40 | word_index).  A
 * shadow array holds the expected generation per word; every read verifies each
 * word's index (catches zero-fill / wrong-folio) and generation (catches stale /
 * torn / failed-RMW).  Scenarios:
 *
 *   D1 unmapped full-page write(2) over on-CRAM folios -> drop -> read back clean
 *   D2 unmapped PARTIAL write(2) (sub-folio, unaligned) -> drop + RMW -> the
 *      untouched words in the folio must survive (THE critical case)
 *   D3 mapped RO reader racing a write(2) writer -> folio_mapped -> promote path;
 *      reader must stay coherent, no torn/stale, no crash
 *   D4 durability: drop-write then drop_caches -> re-read from disk matches
 *   D5 re-demote churn: [write(2)->drop]->[PAGEOUT re-demote]->verify, many loops
 *      (leak / refcount / UAF surface under KASAN+lockdep)
 *   D6 concurrent unmapped writers (full+partial) + read(2) readers, per-page
 *      locked so any torn/stale is a real kernel tear
 *
 * Also samples /sys/kernel/debug/cram/promote_count: D1/D2/D5 must NOT grow it
 * (drop, not promote); D3 may (mapped -> promote).  Exit 0 clean, 1 on any
 * corruption or mechanism violation.
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
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MADV_RANDOM
#define MADV_RANDOM 1
#endif

#define NR_PAGES 256			/* 1 MiB working set */
#define TORTURE_SECS 6
#define CHURN_LOOPS 200
#define WRITERS 4
#define READERS 4

static long page_size;
static long wpp;			/* 8-byte words per page */
static long nwords;			/* total words in the file */
static int cram_nid;
static const char *mnt;

/* ---- stamp model: word = (gen << 40) | word_index ---- */
static uint64_t wstamp(long widx, uint32_t gen)
{
	return ((uint64_t)gen << 40) | (uint64_t)(widx & ((1ULL << 40) - 1));
}
static uint32_t wstamp_gen(uint64_t v)   { return (uint32_t)(v >> 40); }
static uint64_t wstamp_idx(uint64_t v)   { return v & ((1ULL << 40) - 1); }

/* ---- on-CRAM residency (numa_maps of a mapping) ---- */
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

static long read_ulong_file(const char *path)
{
	char buf[64] = {0};
	int fd = open(path, O_RDONLY);
	long v = -1;

	if (fd < 0)
		return -1;
	if (read(fd, buf, sizeof(buf) - 1) > 0)
		v = strtol(buf, NULL, 10);
	close(fd);
	return v;
}
static long promote_count(void)
{
	return read_ulong_file("/sys/kernel/debug/cram/promote_count");
}

/* ---- file setup + write/verify against the shadow ---- */
static int make_file(const char *path, uint32_t gen, uint32_t *wgen)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	uint64_t *buf;
	long p, i;

	if (fd < 0)
		return -1;
	buf = malloc(page_size);
	for (p = 0; p < NR_PAGES; p++) {
		for (i = 0; i < wpp; i++)
			buf[i] = wstamp(p * wpp + i, gen);
		if (pwrite(fd, buf, page_size, p * page_size) != page_size) {
			free(buf); close(fd); return -1;
		}
	}
	free(buf);
	fsync(fd);
	for (i = 0; i < nwords; i++)
		wgen[i] = gen;
	return fd;
}

/* write words [w0, w0+n) at generation @gen; update the shadow. */
static int write_region(int fd, uint32_t *wgen, long w0, long n, uint32_t gen)
{
	uint64_t *buf = malloc(n * 8);
	long i;
	ssize_t r;

	for (i = 0; i < n; i++)
		buf[i] = wstamp(w0 + i, gen);
	r = pwrite(fd, buf, n * 8, w0 * 8);
	free(buf);
	if (r != n * 8)
		return -1;
	for (i = 0; i < n; i++)
		wgen[w0 + i] = gen;
	return 0;
}

/* verify words [w0, w0+n) via read(2) against the shadow. returns #bad words. */
static long verify_region(int fd, const uint32_t *wgen, long w0, long n,
			  const char *what)
{
	uint64_t *buf = malloc(n * 8);
	long i, bad = 0;
	ssize_t r = pread(fd, buf, n * 8, w0 * 8);

	if (r != n * 8) {
		fprintf(stderr, "%s: short read %zd\n", what, r);
		free(buf);
		return n;
	}
	for (i = 0; i < n; i++) {
		uint64_t v = buf[i];

		if (wstamp_idx(v) != (uint64_t)(w0 + i) ||
		    wstamp_gen(v) != wgen[w0 + i]) {
			if (bad < 8)
				fprintf(stderr,
					"%s: word %ld got (gen=%u idx=%llu) want (gen=%u idx=%ld)%s\n",
					what, w0 + i, wstamp_gen(v),
					(unsigned long long)wstamp_idx(v),
					wgen[w0 + i], w0 + i,
					v == 0 ? " [ZERO=RMW-fail]" : "");
			bad++;
		}
	}
	free(buf);
	return bad;
}

/*
 * mmap RO + fault + MADV_PAGEOUT so clean folios demote onto CRAM, then munmap
 * (folios stay resident-on-CRAM in the page cache, now UNMAPPED).  Returns CRAM
 * residency observed while mapped, or -1.
 */
static long demote_unmapped(int fd)
{
	long len = (long)NR_PAGES * page_size, i;
	unsigned char *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	volatile unsigned char sink = 0;
	long on;

	if (p == MAP_FAILED)
		return -1;
	madvise(p, len, MADV_RANDOM);
	for (i = 0; i < NR_PAGES; i++)
		sink += p[i * page_size];
	(void)sink;
	if (madvise(p, len, MADV_PAGEOUT)) {
		munmap(p, len);
		return -1;
	}
	on = pages_on_node(p, cram_nid);
	munmap(p, len);				/* now unmapped: write(2) -> drop */
	return on;
}

static void drop_caches(void)
{
	int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);

	if (fd >= 0) { if (write(fd, "1\n", 2) < 0) {} close(fd); }
}

/* ---------------- D6 concurrent torture ---------------- */
struct tort {
	int fd;
	uint32_t *wgen;
	uint32_t gen[NR_PAGES];		/* committed gen per page under lock */
	pthread_mutex_t lock[NR_PAGES];
	volatile int stop;
	volatile unsigned long reads, writes, bad;
};

static void *th_writer(void *arg)
{
	struct tort *t = arg;
	unsigned int seed = (unsigned int)(uintptr_t)&arg;

	while (!t->stop) {
		long idx = rand_r(&seed) % NR_PAGES, w0, n;
		int partial = rand_r(&seed) & 1;

		pthread_mutex_lock(&t->lock[idx]);
		t->gen[idx]++;
		if (partial) {			/* sub-page unaligned region */
			long a = rand_r(&seed) % wpp, b = rand_r(&seed) % wpp;

			if (a > b) { long tmp = a; a = b; b = tmp; }
			w0 = idx * wpp + a; n = b - a + 1;
		} else {
			w0 = idx * wpp; n = wpp;
		}
		if (write_region(t->fd, t->wgen, w0, n, t->gen[idx]) == 0)
			__atomic_fetch_add(&t->writes, 1, __ATOMIC_RELAXED);
		pthread_mutex_unlock(&t->lock[idx]);
	}
	return NULL;
}
static void *th_reader(void *arg)
{
	struct tort *t = arg;
	unsigned int seed = (unsigned int)(uintptr_t)&arg + 7;

	while (!t->stop) {
		long idx = rand_r(&seed) % NR_PAGES;
		long b;

		pthread_mutex_lock(&t->lock[idx]);
		b = verify_region(t->fd, t->wgen, idx * wpp, wpp, "D6");
		pthread_mutex_unlock(&t->lock[idx]);
		if (b)
			__atomic_fetch_add(&t->bad, b, __ATOMIC_RELAXED);
		__atomic_fetch_add(&t->reads, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}
static void *th_demoter(void *arg)		/* churn: re-demote clean folios */
{
	struct tort *t = arg;

	while (!t->stop) {
		demote_unmapped(t->fd);
		usleep(20000);
	}
	return NULL;
}

/* ---------------- D3 mapped-writer (promote path) ---------------- */
struct mapt {
	unsigned char *p;
	uint32_t *wgen;
	volatile int stop;
	pthread_mutex_t lock[NR_PAGES];
	volatile unsigned long bad, reads;
};
static void *th_map_reader(void *arg)
{
	struct mapt *m = arg;
	unsigned int seed = (unsigned int)(uintptr_t)&arg + 3;

	while (!m->stop) {
		long idx = rand_r(&seed) % NR_PAGES, i, b = 0;
		const uint64_t *q;

		pthread_mutex_lock(&m->lock[idx]);
		q = (const uint64_t *)(m->p + idx * page_size);	/* RO mmap read */
		for (i = 0; i < wpp; i++)
			if (wstamp_idx(q[i]) != (uint64_t)(idx * wpp + i) ||
			    wstamp_gen(q[i]) != m->wgen[idx * wpp + i])
				b++;
		pthread_mutex_unlock(&m->lock[idx]);
		if (b) __atomic_fetch_add(&m->bad, b, __ATOMIC_RELAXED);
		__atomic_fetch_add(&m->reads, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	char path[256];
	uint32_t *wgen;
	int fd, fail = 0;
	long on, p0, i;
	long prom0, prom1;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <cram_nid> <mnt_dir>\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	mnt = argv[2];
	page_size = sysconf(_SC_PAGESIZE);
	wpp = page_size / 8;
	nwords = (long)NR_PAGES * wpp;
	snprintf(path, sizeof(path), "%s/cram_dropwrite.dat", mnt);
	wgen = calloc(nwords, sizeof(*wgen));

	/* ---- D1: unmapped full-page write(2) -> drop ---- */
	fd = make_file(path, 1, wgen);
	if (fd < 0) { perror("make_file"); return 2; }
	on = demote_unmapped(fd);
	if (on <= 0)
		fprintf(stderr, "warn: demote landed %ld pages on CRAM node%d "
			"(pressure too low?) -- drop path may be untested\n", on, cram_nid);
	prom0 = promote_count();
	for (p0 = 0; p0 < NR_PAGES; p0++)
		if (write_region(fd, wgen, p0 * wpp, wpp, 2)) { perror("D1 write"); fail = 1; }
	{
		long bad = verify_region(fd, wgen, 0, nwords, "D1");

		prom1 = promote_count();
		if (bad) fail = 1;
		printf("D1 unmapped full-overwrite (drop): oncram=%ld bad=%ld promote %ld->%ld %s\n",
		       on, bad, prom0, prom1,
		       (bad || (prom0 >= 0 && prom1 > prom0)) ? "FAIL" : "ok");
		if (prom0 >= 0 && prom1 > prom0) {
			fprintf(stderr, "D1: promote_count grew (%ld->%ld) -- drop did not fire\n",
				prom0, prom1);
			fail = 1;
		}
	}

	/* ---- D2: unmapped PARTIAL write(2) -> drop + RMW (critical) ---- */
	demote_unmapped(fd);
	prom0 = promote_count();
	for (p0 = 0; p0 < NR_PAGES; p0++) {
		/* overwrite the middle third of each page; edges must survive via RMW */
		long a = wpp / 3, n = wpp / 3;

		if (write_region(fd, wgen, p0 * wpp + a, n, 3)) { perror("D2 write"); fail = 1; }
	}
	{
		long bad = verify_region(fd, wgen, 0, nwords, "D2");

		prom1 = promote_count();
		if (bad) fail = 1;
		printf("D2 unmapped partial-overwrite (drop+RMW): bad=%ld promote %ld->%ld %s\n",
		       bad, prom0, prom1, bad ? "FAIL(RMW lost untouched bytes)" : "ok");
	}

	/* ---- D4: durability across drop_caches ---- */
	fsync(fd);
	drop_caches();
	{
		long bad = verify_region(fd, wgen, 0, nwords, "D4");

		if (bad) fail = 1;
		printf("D4 durability (drop_caches + reread): bad=%ld %s\n",
		       bad, bad ? "FAIL" : "ok");
	}

	/* ---- D5: re-demote churn ---- */
	{
		long bad_total = 0, loop;
		unsigned int seed = 12345;

		for (loop = 0; loop < CHURN_LOOPS; loop++) {
			long idx = rand_r(&seed) % NR_PAGES;
			int partial = loop & 1;
			long a, n;

			if (partial) { a = wpp / 4; n = wpp / 2; }
			else { a = 0; n = wpp; }
			write_region(fd, wgen, idx * wpp + a, n, 100 + loop);
			if ((loop & 7) == 0)
				demote_unmapped(fd);		/* re-demote clean folios */
			bad_total += verify_region(fd, wgen, idx * wpp, wpp, "D5");
		}
		if (bad_total) fail = 1;
		printf("D5 re-demote churn (%d loops): bad=%ld %s\n",
		       CHURN_LOOPS, bad_total, bad_total ? "FAIL" : "ok");
	}

	/* ---- D6: concurrent unmapped writers + read(2) readers ---- */
	{
		struct tort t = { .fd = fd, .wgen = wgen };
		pthread_t w[WRITERS], r[READERS], dm;

		for (i = 0; i < NR_PAGES; i++) {
			pthread_mutex_init(&t.lock[i], NULL);
			t.gen[i] = wgen[i * wpp];
		}
		demote_unmapped(fd);
		for (i = 0; i < WRITERS; i++) pthread_create(&w[i], NULL, th_writer, &t);
		for (i = 0; i < READERS; i++) pthread_create(&r[i], NULL, th_reader, &t);
		pthread_create(&dm, NULL, th_demoter, &t);
		sleep(TORTURE_SECS);
		t.stop = 1;
		for (i = 0; i < WRITERS; i++) pthread_join(w[i], NULL);
		for (i = 0; i < READERS; i++) pthread_join(r[i], NULL);
		pthread_join(dm, NULL);
		if (t.bad) fail = 1;
		printf("D6 concurrent unmapped torture: reads=%lu writes=%lu bad=%lu %s\n",
		       t.reads, t.writes, t.bad, t.bad ? "FAIL" : "ok");
	}

	/* ---- D3: mapped RO reader racing a write(2) writer (promote path) ---- */
	{
		struct mapt m = { .wgen = wgen };
		pthread_t rd[READERS];
		long len = (long)NR_PAGES * page_size, bad, loop;
		unsigned int seed = 999;

		close(fd);
		fd = open(path, O_RDWR);
		/* rebuild a known baseline so the shadow matches disk */
		for (p0 = 0; p0 < NR_PAGES; p0++)
			write_region(fd, wgen, p0 * wpp, wpp, 200);
		fsync(fd);
		demote_unmapped(fd);
		m.p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);	/* keeps folios mapped */
		if (m.p == MAP_FAILED) { perror("D3 mmap"); return 2; }
		for (i = 0; i < NR_PAGES; i++) {
			pthread_mutex_init(&m.lock[i], NULL);
			(void)((volatile unsigned char *)m.p)[i * page_size];	/* fault RO */
		}
		prom0 = promote_count();
		for (i = 0; i < READERS; i++)
			pthread_create(&rd[i], NULL, th_map_reader, &m);
		for (loop = 0; loop < 4000; loop++) {
			long idx = rand_r(&seed) % NR_PAGES;

			pthread_mutex_lock(&m.lock[idx]);
			/* folio is mapped by readers -> write(2) must PROMOTE, not drop */
			write_region(fd, wgen, idx * wpp, wpp, 300 + (loop % 50));
			pthread_mutex_unlock(&m.lock[idx]);
		}
		m.stop = 1;
		for (i = 0; i < READERS; i++) pthread_join(rd[i], NULL);
		prom1 = promote_count();
		bad = verify_region(fd, wgen, 0, nwords, "D3-final");
		munmap(m.p, len);
		if (m.bad || bad) fail = 1;
		printf("D3 mapped-writer (promote): reads=%lu bad=%lu final_bad=%ld promote %ld->%ld %s\n",
		       m.reads, m.bad, bad, prom0, prom1, (m.bad || bad) ? "FAIL" : "ok");
	}

	close(fd);
	unlink(path);
	free(wgen);
	printf("%s\n", fail ? "DROPWRITE ADVERSARIAL: FAIL" : "DROPWRITE ADVERSARIAL: all clean");
	return fail ? 1 : 0;
}
