// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM read-only tier generic-mm operations battery.
 *
 * Each subtest demotes anonymous memory onto the CRAM node (MADV_PAGEOUT) and
 * then exercises one core mm operation against the resident present-read-only
 * cram folios, checking they behave like ordinary anon folios:
 *
 *   mprotect  - mprotect(RW) must NOT upgrade a cram PTE in place
 *               (can_change_pte_writable); a later write COW-promotes.
 *   dontneed  - MADV_DONTNEED zaps the cram pages; re-read is zero, off-cram.
 *   mremap    - moving the VMA carries the cram pages; contents intact.
 *   madvfree  - MADV_FREE then write cancels the free; write takes effect.
 *   mlock     - mlocking cram pages keeps them readable, no splat.
 *   exit      - a child that demotes then exits frees its cram pages (no leak).
 *   thp       - a PMD-mapped THP demoted to cram COW-promotes on write
 *               (do_huge_pmd_wp_page split + retry).
 *
 * Usage: cram_ops_tool <cram_nid> <subtest>
 * Exit: 0 pass, 1 fail, 2 usage/setup error, 3 environmental (no demote/THP).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MADV_FREE
#define MADV_FREE 8
#endif

#define DEMOTE_COUNT "/sys/kernel/debug/cram/demote_count"
#define PATTERN 0xAB
#define NEWVAL  0xCD
#define NR_PAGES 64

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

/* mmap + pattern + demote.  Returns the region, or NULL on env failure. */
static unsigned char *demote_region(long len)
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

static int t_mprotect(void)
{
	long len = NR_PAGES * page_size;
	unsigned char *p = demote_region(len);
	uint64_t pfn0, pfn1;

	if (!p)
		return 3;
	pfn0 = pfn(p);
	/* mprotect RW must NOT make the cram PTE writable in place. */
	if (mprotect(p, len, PROT_READ))
		return 2;
	if (mprotect(p, len, PROT_READ | PROT_WRITE))
		return 2;
	if (!present(p) || on_node(p, cram_nid) <= 0 || p[0] != PATTERN)
		return 1;	/* upgraded/moved/corrupted by mprotect */
	/* Now a write must COW-promote. */
	p[0] = NEWVAL;
	pfn1 = pfn(p);
	fprintf(stderr, "mprotect: pfn 0x%" PRIx64 "->0x%" PRIx64 " val=%#x\n",
		pfn0, pfn1, p[0]);
	if (p[0] != NEWVAL || !pfn1 || pfn1 == pfn0)
		return 1;
	munmap(p, len);
	return 0;
}

static int t_dontneed(void)
{
	long len = NR_PAGES * page_size;
	unsigned char *p = demote_region(len);
	int i, rc = 0;

	if (!p)
		return 3;
	if (madvise(p, len, MADV_DONTNEED))
		return 2;
	if (on_node(p, cram_nid) != 0)
		rc = 1;			/* cram pages not zapped */
	for (i = 0; i < NR_PAGES; i++)
		if (p[i * page_size] != 0)
			rc = 1;		/* re-read must be zero-fill */
	fprintf(stderr, "dontneed: on_cram=%ld byte0=%#x\n",
		on_node(p, cram_nid), p[0]);
	munmap(p, len);
	return rc;
}

static int t_mremap(void)
{
	long len = NR_PAGES * page_size;
	unsigned char *p = demote_region(len);
	unsigned char *q;
	int i, rc = 0;

	if (!p)
		return 3;
	q = mremap(p, len, len, MREMAP_MAYMOVE);
	if (q == MAP_FAILED)
		return 2;
	for (i = 0; i < NR_PAGES; i++)
		if (q[i * page_size] != PATTERN)
			rc = 1;		/* contents lost in the move */
	fprintf(stderr, "mremap: moved=%d on_cram=%ld byte0=%#x\n",
		q != p, on_node(q, cram_nid), q[0]);
	if (on_node(q, cram_nid) <= 0)
		rc = 1;			/* should still be cram-resident */
	munmap(q, len);
	return rc;
}

static int t_madvfree(void)
{
	long len = NR_PAGES * page_size;
	unsigned char *p = demote_region(len);

	if (!p)
		return 3;
	if (madvise(p, len, MADV_FREE))
		return 2;
	/* A write after MADV_FREE cancels the free and must take effect. */
	p[0] = NEWVAL;
	fprintf(stderr, "madvfree: byte0=%#x\n", p[0]);
	if (p[0] != NEWVAL)
		return 1;
	munmap(p, len);
	return 0;
}

static int t_mlock(void)
{
	long len = NR_PAGES * page_size;
	unsigned char *p = demote_region(len);
	int i, rc = 0;

	if (!p)
		return 3;
	if (mlock(p, len))
		return 2;
	for (i = 0; i < NR_PAGES; i++)
		if (p[i * page_size] != PATTERN)
			rc = 1;		/* mlock must not corrupt reads */
	fprintf(stderr, "mlock: present0=%d on_cram=%ld byte0=%#x\n",
		present(p), on_node(p, cram_nid), p[0]);
	munlock(p, len);
	munmap(p, len);
	return rc;
}

static int t_exit(void)
{
	long len = 256 * page_size;
	long d0, d1;
	pid_t pid;
	int status;

	d0 = read_long(DEMOTE_COUNT);
	pid = fork();
	if (pid < 0)
		return 2;
	if (pid == 0) {
		unsigned char *p = demote_region(len);

		_exit(p ? 0 : 3);	/* exit WITHOUT unmapping the cram region */
	}
	if (waitpid(pid, &status, 0) < 0)
		return 2;
	if (!WIFEXITED(status))
		return 1;		/* child died abnormally tearing down */
	if (WEXITSTATUS(status) == 3)
		return 3;		/* child couldn't demote */
	d1 = read_long(DEMOTE_COUNT);
	fprintf(stderr, "exit: child demoted (count %ld->%ld) and exited clean\n",
		d0, d1);
	/*
	 * The value here is that exit_mmap() zaps the child's cram folios with
	 * no crash/splat (the suite's dmesg scan covers that).  Node-level
	 * leak-freeness is proven separately by cram_node_basic's inflate-to-
	 * present then deflate-all full recovery; MemFree is not a reliable
	 * instant signal here (unmapped anon legitimately lingers on the LRU).
	 */
	return (d1 > d0 && WEXITSTATUS(status) == 0) ? 0 : 1;
}

static int t_thp(void)
{
	long len = 2 * 1024 * 1024;	/* one PMD */
	unsigned char *p, *q;
	long d0, d1;
	uint64_t pfn0, pfn1;

	if (posix_memalign((void **)&q, len, len))
		return 2;
	p = q;
	if (madvise(p, len, MADV_HUGEPAGE))
		return 3;
	memset(p, PATTERN, len);
	/* Need an actual THP: pfn must be 2MB-aligned. */
	if (!present(p) || (pfn(p) & ((len / page_size) - 1)))
		return 3;
	d0 = read_long(DEMOTE_COUNT);
	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");
	d1 = read_long(DEMOTE_COUNT);
	pfn0 = pfn(p);
	if (d1 <= d0 || on_node(p, cram_nid) <= 0)
		return 3;
	/* Write -> do_huge_pmd_wp_page split + per-PTE COW promote. */
	p[0] = NEWVAL;
	pfn1 = pfn(p);
	fprintf(stderr, "thp: on_cram=%ld pfn 0x%" PRIx64 "->0x%" PRIx64 " val=%#x\n",
		on_node(p, cram_nid), pfn0, pfn1, p[0]);
	if (p[0] != NEWVAL || !pfn1 || pfn1 == pfn0)
		return 1;
	free(q);
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

	if (!strcmp(sub, "mprotect")) return t_mprotect();
	if (!strcmp(sub, "dontneed")) return t_dontneed();
	if (!strcmp(sub, "mremap"))   return t_mremap();
	if (!strcmp(sub, "madvfree")) return t_madvfree();
	if (!strcmp(sub, "mlock"))    return t_mlock();
	if (!strcmp(sub, "exit"))     return t_exit();
	if (!strcmp(sub, "thp"))      return t_thp();
	fprintf(stderr, "unknown subtest %s\n", sub);
	return 2;
}
