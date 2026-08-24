// SPDX-License-Identifier: GPL-2.0
/*
 * ZONE_NO_ALLOC: does withdrawing a zone stop placement without disturbing
 * what is already there?
 *
 * The whole sequence runs in one process, holding every mapping until the end.
 * An earlier version drove three phases from the shell and compared the node's
 * free count between them, which measured nothing: each invocation munmapped
 * on exit, so the residents it had placed were gone before the next phase
 * looked, and "residents untouched" passed because there were none.
 *
 *   A  place with the zone open        -- expect pages to land
 *      withdraw the zone
 *   B  place again                     -- expect NOTHING to land
 *      check A's pages are still there -- withdrawing must not purge
 *      restore the zone
 *   C  place again                     -- expect pages to land
 *
 * Usage: cram_noalloc_tool <cram_nid> <MB>
 * Exit 0 = all four hold; 1 = a contract failed; 2 = setup; 3 = environmental.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "cram_demote.h"

#define CRAM_DBG "/sys/kernel/debug/cram"

/* free pages on @nid, column 9 of cram/nodes */
static long cram_free(int nid)
{
	char line[256];
	FILE *f = fopen(CRAM_DBG "/nodes", "r");
	long val = -1;

	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {	/* header */
		fclose(f);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		int n;
		long zr, cr, per, bal, tgt, blk, alw, fre;

		if (sscanf(line, "%d %ld %ld %ld %ld %ld %ld %ld %ld",
			   &n, &zr, &cr, &per, &bal, &tgt, &blk, &alw, &fre) == 9 &&
		    n == nid) {
			val = fre;
			break;
		}
	}
	fclose(f);
	return val;
}

/* balloon pages held on @nid, column 5 of cram/nodes */
static long cram_balloon(int nid)
{
	char line[256];
	FILE *f = fopen(CRAM_DBG "/nodes", "r");
	long val = -1;

	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		int n;
		long zr, cr, per, bal;

		if (sscanf(line, "%d %ld %ld %ld %ld", &n, &zr, &cr, &per, &bal) == 5 &&
		    n == nid) {
			val = bal;
			break;
		}
	}
	fclose(f);
	return val;
}

static int control(const char *fmt, int nid, long n)
{
	char cmd[64];
	int fd = open(CRAM_DBG "/control", O_WRONLY);
	ssize_t rc;

	if (fd < 0)
		return -1;
	rc = snprintf(cmd, sizeof(cmd), fmt, nid, n);
	rc = write(fd, cmd, rc);
	close(fd);
	return rc > 0 ? 0 : -1;
}

static int noalloc(int nid, int on)
{
	char cmd[64];
	int fd = open(CRAM_DBG "/control", O_WRONLY);
	ssize_t rc;

	if (fd < 0)
		return -1;
	rc = snprintf(cmd, sizeof(cmd), "noalloc %d %d", nid, on);
	rc = write(fd, cmd, rc);
	close(fd);
	return rc > 0 ? 0 : -1;
}

/* map, fill, ask for placement; return pages that landed on @nid */
static long place(int nid, size_t len, unsigned char **keep)
{
	long before, after;
	unsigned char *p;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return -1;
	memset(p, 0x6E, len);

	before = cram_free(nid);
	cram_demote(p, len);		/* refusal is a legitimate outcome */
	after = cram_free(nid);

	*keep = p;			/* caller holds it; do not unmap */
	return before - after;
}

int main(int argc, char **argv)
{
	unsigned char *a = NULL, *b = NULL, *c = NULL;
	long placed_a, placed_b, placed_c, held, held2;
	long bal0 = -1, bal1 = -1;
	size_t len;
	int nid, fail = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <nid> <MB>\n", argv[0]);
		return 2;
	}
	nid = atoi(argv[1]);
	len = (size_t)atoi(argv[2]) << 20;

	if (cram_free(nid) < 0) {
		fprintf(stderr, "cram/nodes unreadable for node %d\n", nid);
		return 2;
	}

	/* A: the zone is open */
	placed_a = place(nid, len, &a);
	if (placed_a <= 0) {
		printf("A place-open: nothing landed (%ld) - cannot test\n", placed_a);
		return 3;
	}

	/* withdraw */
	if (noalloc(nid, 1)) {
		fprintf(stderr, "cannot write the noalloc verb\n");
		return 2;
	}
	held = cram_free(nid);

	/* B: nothing may land now */
	placed_b = place(nid, len, &b);

	/* A's residents must still be there */
	held2 = cram_free(nid);

	/*
	 * The owner's own drain must still work while the zone is withdrawn.
	 * This is the whole reason the balloon takes pages with
	 * zone_drain_free_pages() instead of allocating: an allocating balloon
	 * would be refused by the flag it set itself, which is exactly
	 * backwards during a convergence.
	 */
	bal0 = cram_balloon(nid);
	control("inflate %d %ld", nid, 4096);
	bal1 = cram_balloon(nid);
	control("deflate %d %ld", nid, bal1 - bal0 > 0 ? bal1 - bal0 : 0);

	/* restore */
	noalloc(nid, 0);

	/* C: placement works again */
	placed_c = place(nid, len, &c);

	printf("A place-open=%ld  B place-withdrawn=%ld  C place-restored=%ld  resident free %ld->%ld  balloon-while-withdrawn %ld->%ld\n",
	       placed_a, placed_b, placed_c, held, held2, bal0, bal1);

	if (placed_b > 0) {
		printf("FAIL: %ld pages landed on a withdrawn zone\n", placed_b);
		fail = 1;
	}
	if (held2 < held) {
		printf("FAIL: withdrawing the zone purged %ld resident pages\n",
		       held - held2);
		fail = 1;
	}
	if (placed_c <= 0) {
		printf("FAIL: zone did not accept placement again (%ld)\n", placed_c);
		fail = 1;
	}
	if (bal1 <= bal0) {
		printf("FAIL: the owner's own drain was blocked by its own flag (%ld->%ld)\n",
		       bal0, bal1);
		fail = 1;
	}
	if (!fail)
		printf("NOALLOC OK: withdrawn zone refused %zu pages, kept its residents, recovered\n",
		       len / 4096);

	munmap(a, len);
	if (b != MAP_FAILED && b)
		munmap(b, len);
	if (c != MAP_FAILED && c)
		munmap(c, len);
	return fail;
}
