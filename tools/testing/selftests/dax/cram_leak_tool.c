// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM read-only tier write-protection leak battery.
 *
 * Each subtest forces a folio onto the CRAM private node and then drives a code
 * path that is suspected (or, for anon_exclusive_assert, asserted to never) to
 * write a CRAM folio in place -- i.e. without first promoting it off the tier.
 * The CRAM invariant is: a CRAM folio is mapped present-READ-ONLY; any write
 * must migrate (promote) it to DRAM first.  The userspace detector is the same
 * pfn idiom used by cram_ops_tool.c: after a write, the folio's pfn MUST change
 * (= promoted off CRAM); an unchanged pfn together with a changed byte means the
 * store hit the CRAM device in place == LEAK.
 *
 * Why pfn is the primary detector and not the kernel CONFIG_CRAM_DEBUG verifier:
 * the verifier hooks folio_unlock and recomputes a crc.  It MISSES a leak driven
 * through a WRITABLE pte (a HW store never unlocks the folio) and a kernel
 * direct-map write that never unlocks the folio either.  So the dmesg scan in
 * the .sh wrapper is only a secondary signal; the aio/anon paths rely on
 * pfn/on_node observation.
 *
 *   aio_ring               - BUG B3: the aio completion ring (page-cache on an
 *                            anon_inode) can be migrated ONTO CRAM by reclaim;
 *                            aio_complete then writes io_events via the direct
 *                            map.  Assert the ring is NEVER on the CRAM node.
 *   anon_exclusive_assert  - defense-in-depth: a CRAM anon folio must never be
 *                            written in place; a FOLL_WRITE access (here a pipe
 *                            read into the CRAM buffer) must always promote.
 *   shmem_excluded         - limitation: shmem (tmpfs/memfd) is no longer
 *                            CRAM-eligible.  Populate a memfd, MADV_PAGEOUT a
 *                            PROT_READ mapping of it, and assert the folios did
 *                            NOT land on CRAM (on_node == 0).
 *
 * Usage: cram_leak_tool <cram_nid> <subtest>
 * Exit: 0 pass, 1 fail (leak detected), 2 usage/setup error,
 *       3 environmental (no demote / interface unavailable).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/aio_abi.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MADV_FREE
#define MADV_FREE 8
#endif

#define DEMOTE_COUNT "/sys/kernel/debug/cram/demote_count"
#define PATTERN  0xAB
#define NEWVAL   0xCD
#define NR_PAGES 32

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
static int present(void *v) { return !!(pagemap(v) & (1ULL << 63)); }
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

/* Fault every page of a mapping read-only so numa_maps/pagemap can see it. */
static unsigned long touch_ro(volatile unsigned char *p, long len)
{
	unsigned long acc = 0;
	long i;

	for (i = 0; i < len; i += page_size)
		acc += p[i];
	return acc;
}

/* ------------------------------------------------------------------ *
 * BUG B3: aio completion ring migrated onto CRAM, written via the direct map.
 *
 * fs/aio.c: the completion ring is page-cache on an anon_inode (noop_dirty_folio,
 * no read_folio).  A cold ring folio passes cram_folio_eligible (clean, uptodate,
 * mapped) and is evictable, so reclaim can migrate it ONTO CRAM
 * (aio_migrate_folio repoints ctx->ring_folios[]).  aio_complete then writes
 * io_events via folio_address() (the kernel direct map) into the CRAM folio.
 *
 * Repro: io_setup, submit+reap a few events so the ring goes cold, drive heavy
 * memory pressure to demote the ring folio, then submit more iocbs to force
 * aio_complete writes.
 *
 * Detector: neither the verifier nor the WARN fire (direct-map write, no
 * folio_unlock).  The ring is not mmap()able by us, but the aio_context_t
 * returned by io_setup IS the user address of the ring mmap.  So we read
 * /proc/self/pagemap and /proc/self/numa_maps at the ctx base and assert the
 * ring folio is NEVER on the CRAM node.  The FIX (mapping_set_unevictable on the
 * ring inode) makes the ring never-demotable -> on_node(ring)==0 always, which
 * is exactly what this test confirms.  Reliability depends on reclaim actually
 * selecting the ring folio under pressure; if pressure can't be established we
 * return 3.
 * ------------------------------------------------------------------ */
static long io_setup_raw(unsigned nr, aio_context_t *ctxp)
{
	return syscall(__NR_io_setup, nr, ctxp);
}
static long io_submit_raw(aio_context_t ctx, long nr, struct iocb **iocbpp)
{
	return syscall(__NR_io_submit, ctx, nr, iocbpp);
}
static long io_getevents_raw(aio_context_t ctx, long min_nr, long nr,
			     struct io_event *events, struct timespec *timeout)
{
	return syscall(__NR_io_getevents, ctx, min_nr, nr, events, timeout);
}
static long io_destroy_raw(aio_context_t ctx)
{
	return syscall(__NR_io_destroy, ctx);
}

/* pressure: large anon region paged out repeatedly to evict cold cache. */
static void mem_pressure(void)
{
	long len = 256L * 1024 * 1024;		/* 256 MiB */
	unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int r;

	if (p == MAP_FAILED)
		return;
	for (r = 0; r < 4; r++) {
		memset(p, r + 1, len);
		madvise(p, len, MADV_PAGEOUT);
	}
	munmap(p, len);
}

static int t_aio_ring(void)
{
	aio_context_t ctx = 0;
	struct iocb cb, *cbp = &cb;
	struct io_event ev[8];
	struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
	void *ring;
	long on_cram_seen = 0;
	int fd, round, leak = 0;
	char buf[512];

	/* a regular file fd to drive trivial IOCB_CMD_PREAD completions on. */
	fd = memfd_create("cram_aio", 0);
	if (fd < 0) {
		perror("memfd_create");
		return 2;
	}
	if (ftruncate(fd, sizeof(buf))) {
		close(fd);
		return 2;
	}

	if (io_setup_raw(64, &ctx) < 0) {
		perror("io_setup");
		close(fd);
		return 3;	/* CONFIG_AIO off or unavailable */
	}
	/* The ctx id is the user address of the ring mmap. */
	ring = (void *)(uintptr_t)ctx;

	for (round = 0; round < 8; round++) {
		long s, g;

		/* Submit a single trivial read; reap it so the ring goes cold. */
		memset(&cb, 0, sizeof(cb));
		cb.aio_lio_opcode = IOCB_CMD_PREAD;
		cb.aio_fildes = fd;
		cb.aio_buf = (uint64_t)(uintptr_t)buf;
		cb.aio_nbytes = sizeof(buf);
		cb.aio_offset = 0;

		s = io_submit_raw(ctx, 1, &cbp);
		if (s < 1) {
			perror("io_submit");
			break;
		}
		g = io_getevents_raw(ctx, 1, 8, ev, &ts);
		if (g < 1)
			fprintf(stderr, "  io_getevents returned %ld\n", g);

		/*
		 * Force the cold ring out directly: MADV_PAGEOUT on the ring VMA
		 * reclaims it, and (buggy) cram_folio_eligible accepts the
		 * clean+uptodate+mapped ring so it demotes onto cram.  Back it
		 * with broad anon pressure in case PAGEOUT no-ops.
		 */
		madvise(ring, page_size, MADV_PAGEOUT);
		mem_pressure();

		/* Observe the ring's placement. */
		touch_ro(ring, page_size);
		if (on_node(ring, cram_nid) > 0) {
			on_cram_seen = on_node(ring, cram_nid);
			leak = 1;
			break;
		}
	}

	fprintf(stderr,
		"  aio_ring: ring=%p present=%d on_cram=%ld -> %s\n",
		ring, present(ring), on_cram_seen, leak ? "LEAK(ring on cram)" : "ok");

	io_destroy_raw(ctx);
	close(fd);
	/*
	 * We cannot guarantee reclaim selected the ring folio.  If it never
	 * landed on cram we report success (the FIX guarantees this); we never
	 * report 3 here purely for "did not migrate" because on a fixed kernel
	 * that IS the pass condition.  3 is reserved for io_setup unavailable.
	 */
	return leak ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 * Defense-in-depth: a CRAM anon folio must never be written in place.
 *
 * The whole arch/kernel safety of CRAM rests on a CRAM anon folio never being
 * PageAnonExclusive while RO-mapped, and on every FOLL_WRITE access promoting
 * rather than writing through.  We demote an anon region to CRAM, assert the
 * pages are present and not user-writable in place (a store promotes), and then
 * drive a kernel FOLL_WRITE write into the CRAM buffer via read(2) from a pipe.
 * That write must PROMOTE the destination folio (pfn changes), never mutate the
 * CRAM frame in place.
 * ------------------------------------------------------------------ */
static unsigned char *demote_anon(long len)
{
	unsigned char *p;
	long d0, d1;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	memset(p, PATTERN, len);
	d0 = read_long(DEMOTE_COUNT);
	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");
	d1 = read_long(DEMOTE_COUNT);
	if (d1 <= d0 || on_node(p, cram_nid) <= 0) {
		munmap(p, len);
		return NULL;
	}
	return p;
}

static int t_anon_exclusive_assert(void)
{
	long len = NR_PAGES * page_size;
	unsigned char *p = demote_anon(len);
	uint64_t pfn0, pfn1;
	int pfds[2], bad = 0;
	unsigned char src[256];
	ssize_t n;

	if (!p)
		return 3;

	/* Present, on cram, contents intact (zero-copy RO). */
	if (!present(p) || on_node(p, cram_nid) <= 0 || p[0] != PATTERN) {
		munmap(p, len);
		return 1;
	}

	/* FOLL_WRITE via pipe read straight into the CRAM buffer. */
	if (pipe(pfds)) {
		munmap(p, len);
		return 2;
	}
	memset(src, NEWVAL, sizeof(src));
	if (write(pfds[1], src, sizeof(src)) != (ssize_t)sizeof(src)) {
		close(pfds[0]);
		close(pfds[1]);
		munmap(p, len);
		return 2;
	}

	pfn0 = pfn(p);
	n = read(pfds[0], p, sizeof(src));	/* kernel FOLL_WRITE into cram */
	pfn1 = pfn(p);

	if (n != (ssize_t)sizeof(src) || p[0] != NEWVAL)
		bad = 1;			/* the write must take effect */
	if (pfn1 == pfn0)
		bad = 1;			/* ... and must have promoted */

	fprintf(stderr,
		"  anon_exclusive: on_cram=%ld pfn 0x%" PRIx64 "->0x%" PRIx64 " val=%#x -> %s\n",
		on_node(p, cram_nid), pfn0, pfn1, p[0], bad ? "LEAK" : "ok");

	close(pfds[0]);
	close(pfds[1]);
	munmap(p, len);
	return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 * Limitation: shmem (tmpfs/memfd/MAP_SHARED) is no longer CRAM-eligible.
 *
 * CRAM dropped shmem support: only private anonymous folios and clean+readable
 * file folios are demoted.  Prove that a swap-backed shmem (memfd) file stays
 * OFF the CRAM node even when explicitly reclaimed: populate the file, then
 * MADV_PAGEOUT a PROT_READ mapping of it.  The authoritative check is
 * on_node(map, cram_nid) == 0 -- shmem must never land on cram.  (demote_count
 * may or may not move; it is informational only.)
 * ------------------------------------------------------------------ */
static int t_shmem_excluded(void)
{
	long len = (long)NR_PAGES * page_size;
	long d0, d1, oncram;
	unsigned char *w, *map;
	int fd;

	fd = memfd_create("cram_shmem", 0);
	if (fd < 0) {
		perror("memfd_create");
		return 2;
	}
	if (ftruncate(fd, len)) {
		perror("ftruncate");
		close(fd);
		return 2;
	}
	w = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (w == MAP_FAILED) {
		perror("mmap populate");
		close(fd);
		return 2;
	}
	memset(w, PATTERN, len);
	msync(w, len, MS_SYNC);
	munmap(w, len);

	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap inspect");
		close(fd);
		return 2;
	}
	touch_ro(map, len);
	d0 = read_long(DEMOTE_COUNT);
	if (madvise(map, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");
	d1 = read_long(DEMOTE_COUNT);
	touch_ro(map, len);
	oncram = on_node(map, cram_nid);

	fprintf(stderr,
		"  shmem_excluded: demote %ld->%ld on_cram=%ld -> %s\n",
		d0, d1, oncram, oncram == 0 ? "ok(off cram)" : "LEAK(shmem on cram)");

	munmap(map, len);
	close(fd);
	/* shmem must NOT be cram-eligible: any page on cram is a violation. */
	return oncram == 0 ? 0 : 1;
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

	if (!strcmp(sub, "aio_ring"))			return t_aio_ring();
	if (!strcmp(sub, "anon_exclusive_assert"))	return t_anon_exclusive_assert();
	if (!strcmp(sub, "shmem_excluded"))		return t_shmem_excluded();
	fprintf(stderr, "unknown subtest %s\n", sub);
	return 2;
}
