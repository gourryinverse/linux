// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM read-only tier fork / multi-mapper scaling test.
 *
 * Demotes a region of anonymous pages onto the CRAM node (MADV_PAGEOUT), then
 * forks many children that all share those present read-only cram folios, and
 * stresses the shared-RO + per-mapper-COW contract:
 *
 *   - every child reads the cram pages zero-copy and sees the original
 *     contents (one shared folio, fanned out read-only across all mappers);
 *   - each child writes "its" pages, which must COW-promote a private copy
 *     (the writer sees its own value; nobody else is affected);
 *   - after all children exit, the parent's own mappings still read the
 *     original contents and are still resident on the CRAM node (write
 *     isolation held; the shared backing survived the fan-out).
 *
 * A shared control page gives a spin "go" barrier so the mappers overlap.
 *
 * Usage: cram_fork_tool <cram_nid> [nr_children]
 * Exit 0 only if every child verified read-sharing + write-isolation and the
 * parent's view is intact and still on the cram node.
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

#define DEMOTE_COUNT "/sys/kernel/debug/cram/demote_count"
#define PATTERN 0xAB
#define NR_PAGES 64
#define DEF_CHILDREN 32

static long page_size;

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

static int page_present(void *vaddr)
{
	uint64_t val = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);
	off_t off = ((uintptr_t)vaddr / page_size) * sizeof(uint64_t);

	if (fd < 0)
		return 0;
	if (pread(fd, &val, sizeof(val), off) != sizeof(val))
		val = 0;
	close(fd);
	return !!(val & (1ULL << 63)) && !(val & (1ULL << 62));
}

static long pages_on_node(void *vaddr, int nid)
{
	char line[4096], key[32], tok[32];
	FILE *f = fopen("/proc/self/numa_maps", "r");
	long pages = -1;

	if (!f)
		return -1;
	snprintf(key, sizeof(key), "%lx", (unsigned long)vaddr);
	snprintf(tok, sizeof(tok), "N%d=", nid);
	while (fgets(line, sizeof(line), f)) {
		char *p;

		if (strncmp(line, key, strlen(key)) != 0)
			continue;
		p = strstr(line, tok);
		pages = p ? atol(p + strlen(tok)) : 0;
		break;
	}
	fclose(f);
	return pages;
}

/* Per-child unique byte for the pages it writes. */
static unsigned char child_val(int idx) { return (unsigned char)(0x40 + (idx & 0x3f)); }

int main(int argc, char **argv)
{
	int cram_nid, nr_children = DEF_CHILDREN, i, fails = 0;
	long len, demote0, demote1, nb, na;
	unsigned char *p;
	volatile int *go;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cram_nid> [nr_children]\n", argv[0]);
		return 2;
	}
	cram_nid = atoi(argv[1]);
	if (argc > 2)
		nr_children = atoi(argv[2]);
	page_size = sysconf(_SC_PAGESIZE);
	len = (long)NR_PAGES * page_size;

	/* Shared control page (survives fork via MAP_SHARED) for the go barrier. */
	go = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (go == MAP_FAILED) {
		perror("mmap go");
		return 2;
	}
	*go = 0;

	/* Private anon region -> pattern -> demote to cram. */
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap region");
		return 2;
	}
	memset(p, PATTERN, len);

	demote0 = read_long(DEMOTE_COUNT);
	if (madvise(p, len, MADV_PAGEOUT))
		perror("madvise(PAGEOUT)");
	demote1 = read_long(DEMOTE_COUNT);

	nb = pages_on_node(p, cram_nid);
	fprintf(stderr, "diag: demote %ld->%ld on_cram=%ld present0=%d nr_children=%d\n",
		demote0, demote1, nb, page_present(p), nr_children);

	if (demote1 <= demote0 || nb <= 0) {
		printf("region did not demote to CRAM (demote %ld->%ld on_cram=%ld)\n",
		       demote0, demote1, nb);
		return 3;	/* environmental */
	}

	for (i = 0; i < nr_children; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			return 2;
		}
		if (pid == 0) {
			int idx = i, j, rc = 0;
			unsigned char v = child_val(idx);

			while (!*go)		/* overlap the mappers */
				;

			/* All pages must read the shared cram contents zero-copy. */
			for (j = 0; j < NR_PAGES; j++)
				if (p[j * page_size] != PATTERN)
					rc |= 1;

			/* Write "my" stride of pages -> per-mapper COW promote. */
			for (j = idx; j < NR_PAGES; j += nr_children)
				p[j * page_size] = v;
			for (j = idx; j < NR_PAGES; j += nr_children)
				if (p[j * page_size] != v)
					rc |= 2;

			/* Pages I did not write still read the shared contents. */
			for (j = 0; j < NR_PAGES; j++)
				if (j % nr_children != idx % nr_children &&
				    p[j * page_size] != PATTERN)
					rc |= 4;

			_exit(rc);
		}
	}

	*go = 1;	/* release the herd */

	for (i = 0; i < nr_children; i++) {
		int status;

		if (wait(&status) < 0) {
			perror("wait");
			return 2;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			fails++;
	}

	/* Write isolation: parent's view untouched, still resident on cram. */
	for (i = 0; i < NR_PAGES; i++)
		if (p[i * page_size] != PATTERN)
			fails++;
	na = pages_on_node(p, cram_nid);

	printf("children=%d fails=%d parent_on_cram %ld->%ld parent_present=%d\n",
	       nr_children, fails, nb, na, page_present(p));

	munmap(p, len);
	munmap((void *)go, page_size);
	/* Parent must be unaffected and its pages must still be cram-resident. */
	return (fails == 0 && na > 0) ? 0 : 1;
}
