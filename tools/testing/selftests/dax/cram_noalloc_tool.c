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
 * Usage: cram_noalloc_tool <cram_nid> <MB> <no-alloc-path> <balloon-path>
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

static long node_vmstat(int nid, const char *name)
{
	char key[64];
	char line[256];
	char path[128];
	FILE *f;
	long val = -1;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/node/node%d/vmstat", nid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		long value;

		if (sscanf(line, "%63s %ld", key, &value) == 2 &&
		    !strcmp(key, name)) {
			val = value;
			break;
		}
	}
	fclose(f);
	return val;
}

static long cram_free(int nid)
{
	return node_vmstat(nid, "nr_free_pages");
}

static long cram_balloon(int nid)
{
	return node_vmstat(nid, "nr_balloon_pages");
}

static int write_value(const char *path, unsigned long value)
{
	char cmd[64];
	int fd = open(path, O_WRONLY);
	ssize_t rc;

	if (fd < 0)
		return -1;
	rc = snprintf(cmd, sizeof(cmd), "%lu", value);
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
	cram_pageout(p, len);		/* refusal is a legitimate outcome */
	after = cram_free(nid);

	*keep = p;			/* caller holds it; do not unmap */
	return before - after;
}

int main(int argc, char **argv)
{
	unsigned char *a = NULL, *b = NULL, *c = NULL;
	long placed_a, placed_b, placed_c, held, held2;
	long bal0 = -1, bal1 = -1;
	const char *no_alloc_path, *balloon_path;
	size_t len;
	int nid, fail = 0;

	if (argc < 5) {
		fprintf(stderr,
			"usage: %s <nid> <MB> <no-alloc-path> <balloon-path>\n",
			argv[0]);
		return 2;
	}
	nid = atoi(argv[1]);
	len = (size_t)atoi(argv[2]) << 20;
	no_alloc_path = argv[3];
	balloon_path = argv[4];

	if (cram_free(nid) < 0) {
		fprintf(stderr, "node%d vmstat unreadable\n", nid);
		return 2;
	}

	/* A: the zone is open */
	placed_a = place(nid, len, &a);
	if (placed_a <= 0) {
		printf("A place-open: nothing landed (%ld) - cannot test\n", placed_a);
		return 3;
	}

	/* withdraw */
	if (write_value(no_alloc_path, 1)) {
		fprintf(stderr, "cannot withdraw CRAM allocation\n");
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
	 * alloc_contig_range() instead of allocating: an allocating balloon
	 * would be refused by the flag it set itself, which is exactly
	 * backwards during a convergence.
	 */
	bal0 = cram_balloon(nid);
	write_value(balloon_path, bal0 + 4096);
	for (int i = 0; i < 100; i++) {
		bal1 = cram_balloon(nid);
		if (bal1 > bal0)
			break;
		usleep(100000);
	}
	write_value(balloon_path, bal0);

	/* restore */
	write_value(no_alloc_path, 0);

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
