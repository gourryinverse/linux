// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM resident file-tier ADVERSARIAL coherence test.
 *
 * Tries hard to make the resident tier either (a) serve STALE bytes to a reader
 * or (b) let a WRITE leak through to the read-only CRAM device in place.  Every
 * page is stamped with a 64-bit (generation<<32 | page_index) value repeated
 * across the page, so a stale read (old generation), a torn read (mixed
 * generations within a page), or an index mismatch is detectable; a write-leak
 * is caught by the pfn-must-move / folio-must-leave-CRAM invariant (a store that
 * keeps the same physical frame ON the CRAM node wrote the device in place).
 *
 * Subtests (each reports its own pass/fail):
 *   A1 mprotect(PROT_WRITE) on a resident shared mapping, then store -> promote
 *   A2 store to an UNMAPPED-but-resident shared page -> filemap_fault fgp_wr
 *   A3 cross-mapping: write gen via mapping A, read via mapping B -> no stale
 *   A4 O_DIRECT write vs mmap read -> DIO invalidate drops the resident folio
 *   A5 truncate-to-zero + re-extend -> hole reads zero, not stale CRAM bytes
 *   A6 concurrent writer/reader/pageout torture with gen stamps (locked reads,
 *      so any torn/stale read is a real kernel tear, not a userspace race)
 *
 * Usage: cram_coherence_tool <cram_nid> <mnt_dir>
 * Exit 0 all clean, 1 a coherence violation (stale/torn read or write leak),
 * 3 environmental (nothing went resident on CRAM), 2 setup error.
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
#include <pthread.h>
#include <sys/mman.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MADV_RANDOM
#define MADV_RANDOM 1
#endif

#define NR_PAGES 64
#define PROBE 7			/* the page probed by the single-shot subtests */
#define FGPWR_PAGE 5		/* an unmapped-but-resident page for A2 (!= PROBE) */
#define TORTURE_SECS 6
#define WRITERS 4
#define READERS 4

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

static uint64_t stamp_of(uint32_t idx, uint32_t gen)
{
	return ((uint64_t)gen << 32) | idx;
}
static void stamp_page(void *page, uint32_t idx, uint32_t gen)
{
	uint64_t s = stamp_of(idx, gen), *q = page;
	long i;

	for (i = 0; i < page_size / 8; i++)
		q[i] = s;
}
/*
 * Returns 0 if @page is a self-consistent stamp for @idx at generation exactly
 * @gen; else <0: -1 index mismatch, -2 wrong generation, -3 torn (mixed stamps).
 */
static int check_page(const void *page, uint32_t idx, uint32_t gen)
{
	const uint64_t *q = page;
	uint64_t want = stamp_of(idx, gen);
	long i;

	if ((uint32_t)q[0] != idx)
		return -1;
	if ((uint32_t)(q[0] >> 32) != gen)
		return -2;
	for (i = 1; i < page_size / 8; i++)
		if (q[i] != want)
			return -3;
	return 0;
}

static int make_file(const char *path, uint32_t gen)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	void *buf;
	long i;

	if (fd < 0)
		return -1;
	buf = malloc(page_size);
	if (!buf) {
		close(fd);
		return -1;
	}
	for (i = 0; i < NR_PAGES; i++) {
		stamp_page(buf, i, gen);
		if (pwrite(fd, buf, page_size, i * page_size) != page_size) {
			free(buf);
			close(fd);
			return -1;
		}
	}
	free(buf);
	fsync(fd);
	return fd;
}

/*
 * mmap @fd + fault every page in read-only (keeps them clean) + MADV_PAGEOUT so
 * reclaim demotes the clean file folios onto CRAM.  Leaves all PTEs unmapped
 * except a re-faulted probe.  *resident = probe VMA pages now on the CRAM node.
 */
static unsigned char *demote(int fd, int prot, long *lenp, long *resident)
{
	long len = (long)NR_PAGES * page_size, i;
	unsigned char *p = mmap(NULL, len, prot, MAP_SHARED, fd, 0);
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
	sink += p[PROBE * page_size];			/* re-fault probe, RO in place */
	*lenp = len;
	*resident = pages_on_node(p, cram_nid);
	return p;
}

/* ---------------- A6 torture ---------------- */
struct tort {
	unsigned char *p;
	long len;
	volatile int stop;
	uint32_t gen[NR_PAGES];			/* committed gen per page (under wlock) */
	volatile unsigned long bad_stale, bad_torn, reads, writes;
	pthread_mutex_t lock[NR_PAGES];
};

static void *th_writer(void *arg)
{
	struct tort *t = arg;
	unsigned int seed = (unsigned int)(uintptr_t)&arg;

	while (!t->stop) {
		uint32_t idx = rand_r(&seed) % NR_PAGES;
		unsigned char *pg = t->p + (long)idx * page_size;

		pthread_mutex_lock(&t->lock[idx]);
		stamp_page(pg, idx, t->gen[idx] + 1);	/* store (promotes off CRAM) */
		t->gen[idx]++;
		pthread_mutex_unlock(&t->lock[idx]);
		__atomic_fetch_add(&t->writes, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

static void *th_reader(void *arg)
{
	struct tort *t = arg;
	unsigned int seed = (unsigned int)(uintptr_t)&arg + 1;

	while (!t->stop) {
		uint32_t idx = rand_r(&seed) % NR_PAGES;
		unsigned char *pg = t->p + (long)idx * page_size;
		int rc;

		/*
		 * Read under the page lock so no writer is mid-store: the folio must
		 * therefore read back as EXACTLY the committed generation.  Any tear
		 * or wrong gen here is a genuine kernel coherence fault, not a
		 * userspace read/write race.
		 */
		pthread_mutex_lock(&t->lock[idx]);
		rc = check_page(pg, idx, t->gen[idx]);
		pthread_mutex_unlock(&t->lock[idx]);
		if (rc == -2)
			__atomic_fetch_add(&t->bad_stale, 1, __ATOMIC_RELAXED);
		else if (rc == -1 || rc == -3)
			__atomic_fetch_add(&t->bad_torn, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&t->reads, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

static void *th_churn(void *arg)
{
	struct tort *t = arg;

	while (!t->stop) {
		madvise(t->p, t->len, MADV_PAGEOUT);	/* re-demote clean folios to CRAM */
		usleep(2000);
	}
	return NULL;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int torture(const char *mnt)
{
	char path[256];
	struct tort t;
	pthread_t w[WRITERS], r[READERS], c;
	long len, resident;
	double t0;
	int i, rc = 0;

	memset(&t, 0, sizeof(t));
	snprintf(path, sizeof(path), "%s/cram_coh_tort", mnt);
	i = make_file(path, 1);
	if (i < 0)
		return 2;
	for (int k = 0; k < NR_PAGES; k++) {
		pthread_mutex_init(&t.lock[k], NULL);
		t.gen[k] = 1;
	}
	t.p = demote(i, PROT_READ | PROT_WRITE, &len, &resident);
	if (!t.p) {
		close(i);
		return 2;
	}
	t.len = len;
	if (resident <= 0) {
		munmap(t.p, len);
		close(i);
		return 3;
	}
	for (int k = 0; k < WRITERS; k++)
		pthread_create(&w[k], NULL, th_writer, &t);
	for (int k = 0; k < READERS; k++)
		pthread_create(&r[k], NULL, th_reader, &t);
	pthread_create(&c, NULL, th_churn, &t);
	for (t0 = now(); now() - t0 < TORTURE_SECS; )
		usleep(100000);
	t.stop = 1;
	for (int k = 0; k < WRITERS; k++)
		pthread_join(w[k], NULL);
	for (int k = 0; k < READERS; k++)
		pthread_join(r[k], NULL);
	pthread_join(c, NULL);
	munmap(t.p, len);
	close(i);
	if (t.bad_stale || t.bad_torn)
		rc = 1;
	printf("A6 torture: reads=%lu writes=%lu stale=%lu torn=%lu %s\n",
	       t.reads, t.writes, t.bad_stale, t.bad_torn, rc ? "FAIL" : "ok");
	return rc;
}

/* Read the 64-bit stamp at @pg without faulting-in assumptions. */
static uint64_t stamp_at(const void *pg)
{
	return ((const uint64_t *)pg)[0];
}

int main(int argc, char **argv)
{
	const char *mnt;
	char path[256];
	int fd, rc = 0, f, r;
	long len, resident, oncram_before, oncram_after;
	unsigned char *p, *pb;
	uint64_t pfn_before;
	unsigned long probe_off, fgpwr_off;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <cram_nid> <mnt_dir>\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	mnt = argv[2];
	page_size = sysconf(_SC_PAGESIZE);
	probe_off = (unsigned long)PROBE * page_size;
	fgpwr_off = (unsigned long)FGPWR_PAGE * page_size;

	/* ---- A1: mprotect(PROT_WRITE) then store must promote off CRAM ---- */
	f = 0;
	snprintf(path, sizeof(path), "%s/cram_coh_mprot", mnt);
	fd = make_file(path, 1);
	if (fd < 0)
		return 2;
	p = demote(fd, PROT_READ, &len, &resident);
	if (!p) { close(fd); return 2; }
	if (resident <= 0) { munmap(p, len); close(fd); return rc ? 1 : 3; }
	oncram_before = pages_on_node(p, cram_nid);
	pfn_before = page_pfn(&p[probe_off]);
	if (mprotect(p, len, PROT_READ | PROT_WRITE)) { perror("mprotect"); f = 1; }
	stamp_page(&p[probe_off], PROBE, 2);
	oncram_after = pages_on_node(p, cram_nid);
	if (page_pfn(&p[probe_off]) == pfn_before || oncram_after >= oncram_before) {
		fprintf(stderr, "A1: mprotect+store did NOT promote off CRAM "
			"(pfn %llx==%llx oncram %ld->%ld) -- IN-PLACE DEVICE WRITE\n",
			(unsigned long long)pfn_before,
			(unsigned long long)page_pfn(&p[probe_off]),
			oncram_before, oncram_after);
		f = 1;
	}
	if (check_page(&p[probe_off], PROBE, 2)) { fprintf(stderr, "A1 readback\n"); f = 1; }
	printf("A1 mprotect-write: oncram %ld->%ld %s\n",
	       oncram_before, oncram_after, f ? "FAIL" : "ok");
	rc |= f;
	munmap(p, len); close(fd);

	/* ---- A2: store to an UNMAPPED-but-resident shared page -> fgp_wr ---- */
	f = 0;
	snprintf(path, sizeof(path), "%s/cram_coh_fgpwr", mnt);
	fd = make_file(path, 1);
	if (fd < 0) return rc ? 1 : 2;
	p = demote(fd, PROT_READ | PROT_WRITE, &len, &resident);
	if (!p) { close(fd); return rc ? 1 : 2; }
	if (resident <= 0) { munmap(p, len); close(fd); return rc ? 1 : 3; }
	/*
	 * FGPWR_PAGE is mapped read-only on CRAM after demote().  Drive the
	 * page-cache write path -- write(2) -> write_begin ->
	 * __filemap_get_folio(FGP_WRITE) -- so the acquire gate promotes the folio
	 * off CRAM.  This is distinct from A1's mmap store (wp_page_shared()); the
	 * shared mmap must stay coherent as rmap repoints it to the DRAM folio.
	 */
	oncram_before = pages_on_node(p, cram_nid);
	{
		uint64_t s = stamp_of(FGPWR_PAGE, 2), *wb = malloc(page_size);
		long i;

		if (!wb) { munmap(p, len); close(fd); return 2; }
		for (i = 0; i < page_size / 8; i++)
			wb[i] = s;
		if (pwrite(fd, wb, page_size, fgpwr_off) != page_size)
			perror("A2 pwrite");
		free(wb);
	}
	oncram_after = pages_on_node(p, cram_nid);
	if (oncram_after >= oncram_before) {
		fprintf(stderr, "A2: write(2) FGP_WRITE did NOT promote off CRAM "
			"(oncram %ld->%ld)\n", oncram_before, oncram_after);
		f = 1;
	}
	if (check_page(&p[fgpwr_off], FGPWR_PAGE, 2)) { fprintf(stderr, "A2 readback\n"); f = 1; }
	printf("A2 fgpwr-write2: oncram %ld->%ld %s\n",
	       oncram_before, oncram_after, f ? "FAIL" : "ok");
	rc |= f;
	munmap(p, len); close(fd);

	/* ---- A3: cross-mapping -- write via A, read via B must NOT be stale ---- */
	f = 0;
	snprintf(path, sizeof(path), "%s/cram_coh_xmap", mnt);
	fd = make_file(path, 1);
	if (fd < 0) return rc ? 1 : 2;
	p = demote(fd, PROT_READ | PROT_WRITE, &len, &resident);	/* mapping A */
	if (!p) { close(fd); return rc ? 1 : 2; }
	if (resident <= 0) { munmap(p, len); close(fd); return rc ? 1 : 3; }
	pb = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);	/* mapping B */
	if (pb == MAP_FAILED) { munmap(p, len); close(fd); return 2; }
	stamp_page(&p[probe_off], PROBE, 2);			/* write gen2 via A */
	r = check_page(&pb[probe_off], PROBE, 2);		/* read via B */
	if (r) {
		fprintf(stderr, "A3: mapping B STALE (%016llx != gen2 %016llx, rc=%d)\n",
			(unsigned long long)stamp_at(&pb[probe_off]),
			(unsigned long long)stamp_of(PROBE, 2), r);
		f = 1;
	}
	printf("A3 cross-mapping: B-sees-gen%u %s\n",
	       (unsigned)(stamp_at(&pb[probe_off]) >> 32), f ? "FAIL" : "ok");
	rc |= f;
	munmap(pb, len); munmap(p, len); close(fd);

	/* ---- A4: O_DIRECT write must invalidate the resident CRAM folio ---- */
	f = 0;
	snprintf(path, sizeof(path), "%s/cram_coh_dio", mnt);
	fd = make_file(path, 1);
	if (fd < 0) return rc ? 1 : 2;
	p = demote(fd, PROT_READ, &len, &resident);
	if (!p) { close(fd); return rc ? 1 : 2; }
	if (resident <= 0) { munmap(p, len); close(fd); return rc ? 1 : 3; }
	{
		int dfd = open(path, O_RDWR | O_DIRECT);
		void *buf = NULL;
		int did_dio = 0;

		if (dfd >= 0 && !posix_memalign(&buf, page_size, page_size)) {
			stamp_page(buf, PROBE, 2);
			if (pwrite(dfd, buf, page_size, probe_off) == page_size)
				did_dio = 1;
			free(buf);
		}
		if (dfd >= 0)
			close(dfd);
		if (!did_dio) {
			printf("A4 dio-coherence: O_DIRECT unavailable SKIP\n");
		} else {
			if (check_page(&p[probe_off], PROBE, 2)) {
				fprintf(stderr, "A4: mmap read STALE after O_DIRECT "
					"(%016llx != gen2)\n",
					(unsigned long long)stamp_at(&p[probe_off]));
				f = 1;
			}
			printf("A4 dio-coherence: read-gen%u %s\n",
			       (unsigned)(stamp_at(&p[probe_off]) >> 32),
			       f ? "FAIL" : "ok");
		}
	}
	rc |= f;
	munmap(p, len); close(fd);

	/* ---- A5: truncate to 0 + re-extend -> hole reads zero, not stale ---- */
	f = 0;
	snprintf(path, sizeof(path), "%s/cram_coh_trunc", mnt);
	fd = make_file(path, 1);
	if (fd < 0) return rc ? 1 : 2;
	p = demote(fd, PROT_READ, &len, &resident);
	if (!p) { close(fd); return rc ? 1 : 2; }
	if (resident <= 0) { munmap(p, len); close(fd); return rc ? 1 : 3; }
	munmap(p, len);
	if (ftruncate(fd, 0) || ftruncate(fd, len)) { perror("ftruncate"); f = 1; }
	p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { close(fd); return 2; }
	madvise(p, len, MADV_RANDOM);
	if (stamp_at(&p[probe_off]) != 0) {
		fprintf(stderr, "A5: hole read STALE CRAM bytes (%016llx != 0)\n",
			(unsigned long long)stamp_at(&p[probe_off]));
		f = 1;
	}
	printf("A5 truncate-hole: read=%016llx %s\n",
	       (unsigned long long)stamp_at(&p[probe_off]), f ? "FAIL" : "ok");
	rc |= f;
	munmap(p, len); close(fd);

	/* ---- A6: concurrent torture ---- */
	r = torture(mnt);
	if (r == 1)
		rc = 1;
	else if (r == 3 && rc == 0)
		rc = 3;

	return rc;
}
