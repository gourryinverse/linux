// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM read-only anonymous-memory write-protection leak battery.
 *
 * Each subtest forces a folio onto the CRAM private node and then verifies the
 * anonymous-only contract.
 * The CRAM invariant is: a CRAM folio is mapped present-READ-ONLY; any write
 * must migrate (promote) it to DRAM first.  The userspace detector is the same
 * pfn idiom used by cram_ops_tool.c: after a write, the folio's pfn MUST change
 * (= promoted off CRAM); an unchanged pfn together with a changed byte means the
 * store hit the CRAM device in place == LEAK.
 *
 * The pfn is the primary detector because a hardware store through a writable
 * PTE does not necessarily execute any kernel path that could assert the
 * write-fence invariant.  The wrapper's dmesg scan is a secondary signal.
 *
 *   anon_exclusive_assert  - defense-in-depth: a CRAM anon folio must never be
 *                            written in place; a FOLL_WRITE access (here a pipe
 *                            read into the CRAM buffer) must always promote.
 *   shmem_excluded         - limitation: shmem (tmpfs/memfd) is no longer
 *                            CRAM-eligible.  Populate a memfd, MADV_PAGEOUT a
 *                            PROT_READ mapping of it, and assert the folios did
 *                            NOT land on CRAM (on_node == 0).  Also checks the
 *                            folios actually left memory: on_node == 0 is
 *                            vacuous if reclaim never ran.
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
#include "cram_demote.h"

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MADV_FREE
#define MADV_FREE 8
#endif

#define PATTERN  0xAB
#define NEWVAL   0xCD
#define NR_PAGES 32

static long page_size;
static int cram_nid;


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
static long count_present(void *v, long len)
{
	long i, n = 0;

	for (i = 0; i < len; i += page_size)
		n += present((char *)v + i);
	return n;
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

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	memset(p, PATTERN, len);
	if (cram_pageout(p, len))
		perror("madvise(PAGEOUT)");
	if (on_node(p, cram_nid) <= 0) {
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
 * CRAM accepts only private anonymous folios.  Prove that a swap-backed
 * shmem (memfd) file stays
 * OFF the CRAM node even when explicitly reclaimed: populate the file, then
 * MADV_PAGEOUT a PROT_READ mapping of it.  The authoritative check is
	 * on_node(map, cram_nid) == 0 -- shmem must never land on CRAM.
 * ------------------------------------------------------------------ */
static int t_shmem_excluded(void)
{
	long len = (long)NR_PAGES * page_size;
	long before, after, oncram;
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
	before = count_present(map, len);
	if (cram_pageout(map, len))
		perror("madvise(PAGEOUT)");
	/* Sample before re-faulting: the re-fault would hide what reclaim did. */
	after = count_present(map, len);
	touch_ro(map, len);
	oncram = on_node(map, cram_nid);

	fprintf(stderr,
		"  shmem_excluded: present %ld->%ld (reclaimed %ld) on_cram=%ld -> %s\n",
		before, after, before - after, oncram,
		oncram ? "LEAK(shmem on cram)" :
		(before - after < before / 2) ? "INCONCLUSIVE(no reclaim)" :
		"ok(reclaimed, off cram)");

	munmap(map, len);
	close(fd);

	if (oncram)
		return 1;	/* shmem is not cram-eligible: any page is a leak */
	return 0;
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

	if (!strcmp(sub, "anon_exclusive_assert"))
		return t_anon_exclusive_assert();
	if (!strcmp(sub, "shmem_excluded"))
		return t_shmem_excluded();
	fprintf(stderr, "unknown subtest %s\n", sub);
	return 2;
}
