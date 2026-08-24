// SPDX-License-Identifier: GPL-2.0
/*
 * CRAM convergence-under-hot-memory test.
 *
 * The balloon reserves by allocating, so the only reclaim it gets is the
 * allocator's, and that honours references.  Fill the node with memory that is
 * actively being read and polite reclaim returns FOLIOREF_ACTIVATE for every
 * folio: the allocation fails and the balloon stops with the node still full.
 *
 * That is the right answer for a fixed-size tier and the wrong one for CRAM.
 * When the driver reports a worse compression ratio the physical backing is
 * ALREADY gone, so a balloon that cannot reach its target leaves the device
 * overcommitted.  The convergence worker escalates: forced reclaim ignoring
 * references, then evacuation to DRAM, then a node-scoped OOM kill.
 *
 * This drives the first two rungs.  It demotes anonymous memory onto the node,
 * keeps it hot with a reader child, worsens the ratio, and asserts the balloon
 * still converges -- and that the data survives, since forcing eviction must
 * not corrupt anything.
 *
 * Usage: cram_converge_tool <cram_nid> <ratio_knob_path> <worse_ratio> [secs]
 * Exit 0 = converged with data intact; 3 = environmental (could not demote);
 * 1 = contract failed; 2 = setup error.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "cram_demote.h"

#define CRAM_DBG	"/sys/kernel/debug/cram"
#define PATTERN		0x5C

static long page_size;

/* Read column @col (by header name) for node @nid from cram/nodes. */
static long cram_node_col(int nid, const char *col)
{
	char line[256], hdr[256];
	FILE *f = fopen(CRAM_DBG "/nodes", "r");
	int idx = -1, i;
	long val = -1;
	char *tok;

	if (!f)
		return -1;
	if (!fgets(hdr, sizeof(hdr), f)) {
		fclose(f);
		return -1;
	}
	for (i = 0, tok = strtok(hdr, " \n"); tok; tok = strtok(NULL, " \n"), i++)
		if (!strcmp(tok, col))
			idx = i;
	if (idx < 0) {
		fclose(f);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		int n;

		if (sscanf(line, "%d", &n) == 1 && n == nid) {
			for (i = 0, tok = strtok(line, " \n"); tok;
			     tok = strtok(NULL, " \n"), i++)
				if (i == idx)
					val = atol(tok);
			break;
		}
	}
	fclose(f);
	return val;
}

static int write_knob(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	ssize_t rc;

	if (fd < 0)
		return -1;
	rc = write(fd, val, strlen(val));
	close(fd);
	return rc > 0 ? 0 : -1;
}

/* Keep @p hot: reads are served in place, so they age the folio without
 * promoting it off the node the way a write would.
 */
static void toucher(unsigned char *p, size_t len)
{
	volatile unsigned char sink = 0;

	for (;;) {
		size_t off;

		for (off = 0; off < len; off += page_size)
			sink += p[off];
		(void)sink;
	}
}

int main(int argc, char **argv)
{
	long perceived, free0, free1, bal0, bal1, target, oncram;
	long forced0, forced1;
	unsigned char *p;
	size_t len, off;
	int nid, secs, i, bad = 0;
	const char *knob;
	pid_t child;

	if (argc < 4) {
		fprintf(stderr, "usage: %s <nid> <ratio_knob> <worse_ratio> [secs]\n",
			argv[0]);
		return 2;
	}
	nid = atoi(argv[1]);
	knob = argv[2];
	secs = argc > 4 ? atoi(argv[4]) : 60;
	page_size = sysconf(_SC_PAGESIZE);

	perceived = cram_node_col(nid, "perceived");
	free0 = cram_node_col(nid, "free");
	if (perceived <= 0 || free0 <= 0) {
		fprintf(stderr, "cram/nodes unreadable for node %d\n", nid);
		return 2;
	}

	/*
	 * The balloon only has to reclaim if it cannot satisfy its target out
	 * of free pages, so the resident set must be bigger than
	 * free - target.  Half the node is NOT enough: at zratio 2000 reported
	 * as 1200 the target is 0.4 * perceived, so filling half left more free
	 * than the target and the balloon converged without reclaiming a thing
	 * (forced=0 evacuated=0).  Take three quarters and leave the rest as
	 * headroom for the demote itself.
	 */
	len = (size_t)(free0 - free0 / 4) * page_size;
	if (len > (size_t)1 << 30)
		len = (size_t)1 << 30;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(p, PATTERN, len);

	if (cram_demote(p, len)) {
		fprintf(stderr, "demote verb unavailable\n");
		return 3;
	}
	free1 = cram_node_col(nid, "free");
	oncram = free0 - free1;
	if (oncram <= 0) {
		fprintf(stderr, "nothing landed on node %d (free %ld -> %ld)\n",
			nid, free0, free1);
		return 3;
	}

	/*
	 * Re-warm.  cram_demote() leaves the range MADV_COLD so the generic
	 * path will take it; this test is about the case where it will NOT, so
	 * put the references back before asking the balloon to reclaim.
	 */
	for (i = 0; i < 3; i++)
		for (off = 0; off < len; off += page_size)
			((volatile unsigned char *)p)[off];

	child = fork();
	if (child == 0) {
		toucher(p, len);
		_exit(0);
	}
	if (child < 0) {
		perror("fork");
		return 2;
	}

	bal0 = cram_node_col(nid, "balloon");
	forced0 = cram_node_vmstat(nid, "pgcontig_reclaim");

	/* Worsen the ratio: the driver has lost backing, converge or overcommit. */
	if (write_knob(knob, argv[3])) {
		fprintf(stderr, "cannot write %s\n", knob);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 2;
	}

	target = 0;
	bal1 = bal0;
	for (i = 0; i < secs * 10; i++) {
		usleep(100000);
		target = cram_node_col(nid, "target");
		bal1 = cram_node_col(nid, "balloon");
		if (target > 0 && bal1 >= target)
			break;
	}

	forced1 = cram_node_vmstat(nid, "pgcontig_reclaim");

	kill(child, SIGKILL);
	waitpid(child, NULL, 0);

	/* Forcing eviction must not corrupt anything. */
	for (off = 0; off < len; off += page_size)
		if (p[off] != PATTERN)
			bad++;

	printf("converge: oncram=%ld target=%ld bal %ld->%ld free %ld->%ld esc=%ld bad=%d\n",
	       oncram, target, bal0, bal1, free1, cram_node_col(nid, "free"),
	       forced1 - forced0, bad);

	if (bad) {
		printf("CONVERGE FAIL: %d pages corrupted\n", bad);
		return 1;
	}
	if (target <= 0) {
		printf("CONVERGE SKIP: ratio change set no target\n");
		return 3;
	}
	if (bal1 < target) {
		printf("CONVERGE FAIL: balloon stalled at %ld of %ld with hot memory resident\n",
		       bal1, target);
		return 1;
	}
	/*
	 * Escalation is REPORTED, not required.  alloc_contig_range() clears a
	 * block by migrating its residents, and while DRAM has room that
	 * succeeds without ever reaching ACR_FLAGS_RECLAIM -- a legitimate
	 * outcome, and not one this harness can rule out, since it does not
	 * create DRAM pressure.  The claim here is that the balloon reaches
	 * target against a hot resident set; the eviction rung is covered
	 * deterministically by cram_swap, which asks for it directly.
	 */
	printf("CONVERGE OK: balloon reached %ld/%ld against a hot resident set (escalated=%ld)\n",
	       bal1, target, forced1 - forced0);
	return 0;
}
