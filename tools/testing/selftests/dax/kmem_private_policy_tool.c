// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <linux/mempolicy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest.h"

#define NR_PAGES 64
#define PATTERN 0xab
#define HUGE_LEN (2UL << 20)
#define NR_THPS 8

static long page_size;

static long pages_on_node(void *addr, int nid)
{
	char line[4096], key[32], token[32];
	FILE *file = fopen("/proc/self/numa_maps", "r");
	long pages = -1;

	if (!file)
		return -1;
	snprintf(key, sizeof(key), "%lx", (unsigned long)addr);
	snprintf(token, sizeof(token), "N%d=", nid);
	while (fgets(line, sizeof(line), file)) {
		char *pos;

		if (strncmp(line, key, strlen(key)))
			continue;
		pos = strstr(line, token);
		pages = pos ? atol(pos + strlen(token)) : 0;
		break;
	}
	fclose(file);
	return pages;
}

static void *anon_map(size_t len)
{
	void *ptr = mmap(NULL, len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return ptr == MAP_FAILED ? NULL : ptr;
}

static long pages_reported_on_node(void *addr, int nid)
{
	void *pages[NR_PAGES];
	int status[NR_PAGES];
	long i, rc, count = 0;

	for (i = 0; i < NR_PAGES; i++)
		pages[i] = addr + i * page_size;
	rc = syscall(SYS_move_pages, 0, NR_PAGES, pages, NULL, status, 0UL);
	if (rc)
		return -1;
	for (i = 0; i < NR_PAGES; i++)
		count += status[i] == nid;
	return count;
}

#define PARTIAL_STAT \
	"/sys/kernel/mm/transparent_hugepage/hugepages-2048kB/stats/" \
	"nr_anon_partially_mapped"

static long read_long(const char *path)
{
	FILE *file = fopen(path, "r");
	long val = -1;

	if (file) {
		if (fscanf(file, "%ld", &val) != 1)
			val = -1;
		fclose(file);
	}
	return val;
}

static void write_reclaim(const char *path)
{
	FILE *file = fopen(path, "w");

	if (!file)
		return;
	fputs("64M", file);
	fclose(file);
}

/*
 * Reclaim through @path splits partially unmapped THPs on a private node,
 * returning the unmapped subpages. 2 reports that THPs were unavailable on
 * the node.
 */
static int thp_split_reclaimed(int nid, const char *path)
{
	unsigned long mask = 1UL << nid;
	long before, partial;
	char *map, *buf;
	size_t len = NR_THPS * HUGE_LEN;
	int i;

	before = read_long(PARTIAL_STAT);
	map = anon_map(len + HUGE_LEN);
	if (before < 0 || !map)
		return before < 0 ? 2 : 0;
	buf = (char *)(((unsigned long)map + HUGE_LEN - 1) & ~(HUGE_LEN - 1));
	if (madvise(buf, len, MADV_HUGEPAGE) ||
	    syscall(SYS_mbind, buf, len, MPOL_BIND | MPOL_F_PRIVATE_NODES,
		    &mask, 8 * sizeof(mask), 0UL)) {
		munmap(map, len + HUGE_LEN);
		return 2;
	}
	memset(buf, PATTERN, len);
	for (i = 0; i < NR_THPS; i++)
		madvise(buf + i * HUGE_LEN + HUGE_LEN / 2, HUGE_LEN / 2,
			MADV_DONTNEED);

	partial = read_long(PARTIAL_STAT) - before;
	if (partial < NR_THPS) {
		munmap(map, len + HUGE_LEN);
		return 2;
	}
	write_reclaim(path);
	partial = read_long(PARTIAL_STAT) - before;

	munmap(map, len + HUGE_LEN);
	return partial <= NR_THPS / 4;
}

static int swap_active(void)
{
	char line[256];
	FILE *file = fopen("/proc/swaps", "r");
	int lines = 0;

	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file))
		lines++;
	fclose(file);
	return lines > 1;
}

/* Reclaim advice pages out private memory; 2 reports no active swap. */
static int pageout_acts(void *addr, size_t len, unsigned long mask, int nid)
{
	if (!swap_active())
		return 2;
	if (syscall(SYS_mbind, addr, len, MPOL_BIND | MPOL_F_PRIVATE_NODES,
		    &mask, 8 * sizeof(mask), 0UL))
		return 0;
	memset(addr, PATTERN, len);
	if (pages_reported_on_node(addr, nid) != NR_PAGES ||
	    madvise(addr, len, MADV_PAGEOUT))
		return 0;
	return pages_reported_on_node(addr, nid) == 0;
}

/* Find memory.reclaim of the non-root cgroup this task runs in. */
static int memcg_reclaim_path(char *path, size_t size)
{
	char line[4096], *cgroup = NULL;
	FILE *file = fopen("/proc/self/cgroup", "r");

	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file)) {
		if (!strncmp(line, "0::/", 4)) {
			line[strcspn(line, "\n")] = 0;
			cgroup = line + 3;
			break;
		}
	}
	fclose(file);
	if (!cgroup || !strcmp(cgroup, "/"))
		return 0;
	snprintf(path, size, "/sys/fs/cgroup%s/memory.reclaim", cgroup);
	return !access(path, W_OK);
}

static int common_policy(void *addr, size_t len, int nid, int requested_mode,
			 unsigned long mask, int task_policy)
{
	unsigned long reported = 0;
	int mode = 0;
	long rc;

	if (task_policy) {
		if (syscall(SYS_mbind, addr, len, MPOL_DEFAULT, NULL, 0UL, 0UL))
			return 0;
		rc = syscall(SYS_set_mempolicy, requested_mode, &mask,
			     8 * sizeof(mask));
	} else {
		rc = syscall(SYS_mbind, addr, len, requested_mode, &mask,
			     8 * sizeof(mask), 0UL);
	}
	if (rc)
		return 0;

	rc = syscall(SYS_get_mempolicy, &mode, &reported, 8 * sizeof(reported),
		     task_policy ? NULL : addr, task_policy ? 0 : MPOL_F_ADDR);
	if (rc || mode != requested_mode || reported != mask ||
	    madvise(addr, len, MADV_DONTNEED))
		return 0;
	memset(addr, PATTERN, len);
	return pages_reported_on_node(addr, nid) == 0;
}

/* The flag binds exactly one private node and excludes the other modes. */
static int private_flag_validated(void *addr, size_t len,
				  unsigned long private_mask,
				  unsigned long common_mask)
{
	const struct {
		int mode;
		unsigned long mask;
	} cases[] = {
		{ MPOL_BIND | MPOL_F_PRIVATE_NODES | MPOL_F_STATIC_NODES,
		  private_mask },
		{ MPOL_BIND | MPOL_F_PRIVATE_NODES | MPOL_F_RELATIVE_NODES,
		  private_mask },
		{ MPOL_PREFERRED | MPOL_F_PRIVATE_NODES, private_mask },
		{ MPOL_BIND | MPOL_F_PRIVATE_NODES, common_mask },
		{ MPOL_BIND | MPOL_F_PRIVATE_NODES, private_mask | common_mask },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		errno = 0;
		if (syscall(SYS_mbind, addr, len, cases[i].mode, &cases[i].mask,
			    8 * sizeof(cases[i].mask), 0UL) != -1 ||
		    errno != EINVAL)
			return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	unsigned long private_mask, mixed_mask, allowed = 0;
	long len, rc, private_pages, common_pages;
	void *ordinary, *bound, *probe, *mixed, *unflagged, *paged, *huge;
	char path[4096];
	void *pages[1];
	int nodes[1], status, mode;
	int nid, common;
	int default_isolated, mems_visible;
	int policy_rejected, bind_accepted, mixed_narrowed;
	int unflagged_private_rejected, flag_validated;
	int target_allowed, private_visible, move_allowed, lock_stable;
	int pageout;
	int private_flag_reported;
	int relative_task_common, relative_bind_common, static_mask_preserved;
	int preferred_private_rejected, hugetlb_private_rejected;
	int thp_split, memcg_split;
	int memless = -1, private_exact;

	if (argc != 3 && argc != 4) {
		fprintf(stderr,
			"usage: %s <private-nid> <common-nid> [memoryless-nid]\n",
			argv[0]);
		return 2;
	}
	nid = atoi(argv[1]);
	common = atoi(argv[2]);
	if (argc == 4)
		memless = atoi(argv[3]);
	if (nid < 0 || common < 0 || nid >= (int)(8 * sizeof(unsigned long)) ||
	    common >= (int)(8 * sizeof(unsigned long)) ||
	    memless >= (int)(8 * sizeof(unsigned long)))
		return 3;

	page_size = sysconf(_SC_PAGESIZE);
	len = NR_PAGES * page_size;
	private_mask = 1UL << nid;
	mixed_mask = private_mask | (1UL << common);

	ordinary = anon_map(len);
	bound = anon_map(len);
	probe = anon_map(len);
	mixed = anon_map(len);
	unflagged = anon_map(len);
	paged = anon_map(len);
	if (!ordinary || !bound || !probe || !mixed || !unflagged || !paged)
		return 2;
	memset(ordinary, PATTERN, len);
	default_isolated = pages_reported_on_node(ordinary, nid) == 0;

	rc = syscall(SYS_get_mempolicy, &mode, &allowed,
		     8 * sizeof(allowed), NULL, MPOL_F_MEMS_ALLOWED);
	mems_visible = !rc && (allowed & private_mask);

	/* Relative bits are positions in the common-node set, not node IDs. */
	relative_task_common = common_policy(probe, len, nid,
			MPOL_BIND | MPOL_F_RELATIVE_NODES, private_mask, 1);
	syscall(SYS_set_mempolicy, MPOL_DEFAULT, NULL, 0UL);
	relative_bind_common = common_policy(probe, len, nid,
			MPOL_BIND | MPOL_F_RELATIVE_NODES, private_mask, 0);

	/* Filtering the effective task policy must not alter its user mask. */
	static_mask_preserved = common_policy(probe, len, nid,
			MPOL_BIND | MPOL_F_STATIC_NODES, mixed_mask, 1);
	syscall(SYS_set_mempolicy, MPOL_DEFAULT, NULL, 0UL);

	errno = 0;
	rc = syscall(SYS_mbind, probe, len, MPOL_PREFERRED, &private_mask,
		     8 * sizeof(private_mask), 0UL);
	preferred_private_rejected = rc == -1 && errno == EINVAL;

	/* A private bind names one node; 2 reports no memoryless node to add. */
	if (memless < 0) {
		private_exact = 2;
	} else {
		unsigned long extra = private_mask | (1UL << memless);

		errno = 0;
		rc = syscall(SYS_mbind, probe, len,
			     MPOL_BIND | MPOL_F_PRIVATE_NODES, &extra,
			     8 * sizeof(extra), 0UL);
		private_exact = rc == -1 && errno == EINVAL;
	}
	flag_validated = private_flag_validated(probe, len, private_mask,
						1UL << common);

	/* Private nodes cannot hold HugeTLB pages; 2 reports "unavailable". */
	huge = mmap(NULL, HUGE_LEN, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_NORESERVE,
		    -1, 0);
	if (huge == MAP_FAILED) {
		hugetlb_private_rejected = 2;
	} else {
		errno = 0;
		rc = syscall(SYS_mbind, huge, HUGE_LEN,
			     MPOL_BIND | MPOL_F_PRIVATE_NODES, &private_mask,
			     8 * sizeof(private_mask), 0UL);
		hugetlb_private_rejected = rc == -1 && errno == EINVAL;
		munmap(huge, HUGE_LEN);
	}

	snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/reclaim",
		 nid);
	thp_split = thp_split_reclaimed(nid, path);

	errno = 0;
	rc = syscall(SYS_set_mempolicy,
		     MPOL_BIND | MPOL_F_PRIVATE_NODES, &private_mask,
		     8 * sizeof(private_mask));
	policy_rejected = rc == -1 && errno == EINVAL;
	syscall(SYS_set_mempolicy, MPOL_DEFAULT, NULL, 0UL);

	errno = 0;
	rc = syscall(SYS_mbind, bound, len,
		     MPOL_BIND | MPOL_F_PRIVATE_NODES, &private_mask,
		     8 * sizeof(private_mask), 0UL);
	bind_accepted = !rc;

	mode = 0;
	rc = syscall(SYS_get_mempolicy, &mode, NULL, 0UL, bound, MPOL_F_ADDR);
	private_flag_reported = !rc &&
		mode == (MPOL_BIND | MPOL_F_PRIVATE_NODES);

	/* A mixed mask is an ordinary binding narrowed to common memory. */
	rc = syscall(SYS_mbind, mixed, len, MPOL_BIND, &mixed_mask,
		     8 * sizeof(mixed_mask), 0UL);
	memset(mixed, PATTERN, len);
	mixed_narrowed = !rc && pages_on_node(mixed, common) == NR_PAGES;

	/* Without the flag, a private-only mask has no usable node. */
	errno = 0;
	rc = syscall(SYS_mbind, unflagged, len, MPOL_BIND, &private_mask,
		     8 * sizeof(private_mask), 0UL);
	unflagged_private_rejected = rc == -1 && errno == EINVAL;

	memset(bound, PATTERN, len);
	private_pages = pages_on_node(bound, nid);
	common_pages = pages_on_node(bound, common);
	bind_accepted &= private_pages == NR_PAGES && common_pages == 0;

	pageout = pageout_acts(paged, len, private_mask, nid);

	pages[0] = ordinary;
	nodes[0] = nid;
	status = 0;
	errno = 0;
	rc = syscall(SYS_move_pages, 0, 1UL, pages, nodes, &status,
		     MPOL_MF_MOVE);
	target_allowed = !rc && pages_on_node(ordinary, nid) == 1;

	pages[0] = bound;
	status = 0;
	rc = syscall(SYS_move_pages, 0, 1UL, pages, NULL, &status, 0UL);
	private_visible = !rc && status == nid;

	nodes[0] = common;
	status = 0;
	rc = syscall(SYS_move_pages, 0, 1UL, pages, nodes, &status,
		     MPOL_MF_MOVE);
	move_allowed = !rc && pages_on_node(bound, common) == 1;

	rc = mlock(bound, len);
	lock_stable = !rc && pages_on_node(bound, common) == 1;
	if (!rc)
		munlock(bound, len);

	/* memcg reclaim may page out the mappings above, so it runs last. */
	memcg_split = memcg_reclaim_path(path, sizeof(path)) ?
		      thp_split_reclaimed(nid, path) : 2;

	printf("default_isolated=%d\n", default_isolated);
	printf("mems_visible=%d\n", mems_visible);
	printf("policy_rejected=%d\n", policy_rejected);
	printf("bind_accepted=%d\n", bind_accepted);
	printf("mixed_narrowed=%d\n", mixed_narrowed);
	printf("unflagged_private_rejected=%d\n", unflagged_private_rejected);
	printf("private_flag_validated=%d\n", flag_validated);
	printf("target_allowed=%d\n", target_allowed);
	printf("private_visible=%d\n", private_visible);
	printf("move_allowed=%d\n", move_allowed);
	printf("lock_stable=%d\n", lock_stable);
	printf("pageout_acts=%d\n", pageout);
	printf("private_flag_reported=%d\n", private_flag_reported);
	printf("relative_task_common=%d\n", relative_task_common);
	printf("relative_bind_common=%d\n", relative_bind_common);
	printf("static_mask_preserved=%d\n", static_mask_preserved);
	printf("preferred_private_rejected=%d\n", preferred_private_rejected);
	printf("hugetlb_private_rejected=%d\n", hugetlb_private_rejected);
	printf("thp_split_reclaimed=%d\n", thp_split);
	printf("memcg_split_reclaimed=%d\n", memcg_split);
	printf("private_mask_exact=%d\n", private_exact);

	munmap(paged, len);
	munmap(probe, len);
	munmap(bound, len);
	munmap(ordinary, len);
	return 0;
}
