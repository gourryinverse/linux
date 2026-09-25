// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/mempolicy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NR_PAGES 64
#define PATTERN 0xab

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

int main(int argc, char **argv)
{
	unsigned long private_mask, mixed_mask, allowed = 0;
	long len, rc, private_pages, common_pages;
	void *ordinary, *bound, *device;
	void *pages[1];
	int nodes[1], status, mode, fd;
	int nid, common;
	int default_isolated, mems_visible;
	int policy_rejected, bind_rejected, mixed_narrowed;
	int target_rejected, private_hidden, move_blocked, lock_stable;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <device> <private-nid> <common-nid>\n",
			argv[0]);
		return 2;
	}
	nid = atoi(argv[2]);
	common = atoi(argv[3]);
	if (nid < 0 || common < 0 || nid >= (int)(8 * sizeof(unsigned long)) ||
	    common >= (int)(8 * sizeof(unsigned long)))
		return 3;

	page_size = sysconf(_SC_PAGESIZE);
	len = NR_PAGES * page_size;
	private_mask = 1UL << nid;
	mixed_mask = private_mask | (1UL << common);

	ordinary = anon_map(len);
	bound = anon_map(len);
	if (!ordinary || !bound)
		return 2;
	memset(ordinary, PATTERN, len);
	default_isolated = pages_reported_on_node(ordinary, nid) == 0;

	rc = syscall(SYS_get_mempolicy, &mode, &allowed,
		     8 * sizeof(allowed), NULL, MPOL_F_MEMS_ALLOWED);
	mems_visible = !rc && (allowed & private_mask);

	errno = 0;
	rc = syscall(SYS_set_mempolicy, MPOL_BIND, &private_mask,
		     8 * sizeof(private_mask));
	policy_rejected = rc == -1 && errno == EINVAL;
	syscall(SYS_set_mempolicy, MPOL_DEFAULT, NULL, 0UL);

	errno = 0;
	rc = syscall(SYS_mbind, bound, len, MPOL_BIND, &private_mask,
		     8 * sizeof(private_mask), 0UL);
	bind_rejected = rc == -1 && errno == EINVAL;

	rc = syscall(SYS_mbind, bound, len, MPOL_BIND, &mixed_mask,
		     8 * sizeof(mixed_mask), 0UL);
	memset(bound, PATTERN, len);
	private_pages = pages_on_node(bound, nid);
	common_pages = pages_on_node(bound, common);
	mixed_narrowed = !rc && private_pages == 0 && common_pages == NR_PAGES;

	pages[0] = bound;
	nodes[0] = nid;
	status = 0;
	errno = 0;
	rc = syscall(SYS_move_pages, 0, 1UL, pages, nodes, &status,
		     MPOL_MF_MOVE);
	target_rejected = rc == -1 && errno == ENODEV &&
		pages_on_node(bound, nid) == 0;

	fd = open(argv[1], O_RDWR);
	if (fd < 0)
		return 2;
	device = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (device == MAP_FAILED)
		return 2;
	memset(device, PATTERN, len);

	pages[0] = device;
	status = 0;
	rc = syscall(SYS_move_pages, 0, 1UL, pages, NULL, &status, 0UL);
	private_hidden = !rc && status == -ENOENT;

	nodes[0] = common;
	status = 0;
	rc = syscall(SYS_move_pages, 0, 1UL, pages, nodes, &status,
		     MPOL_MF_MOVE);
	move_blocked = !rc && status == -ENOENT &&
		pages_on_node(device, nid) == NR_PAGES;

	rc = mlock(device, len);
	lock_stable = !rc && pages_on_node(device, nid) == NR_PAGES;
	if (!rc)
		munlock(device, len);

	printf("default_isolated=%d\n", default_isolated);
	printf("mems_visible=%d\n", mems_visible);
	printf("policy_rejected=%d\n", policy_rejected);
	printf("bind_rejected=%d\n", bind_rejected);
	printf("mixed_narrowed=%d\n", mixed_narrowed);
	printf("target_rejected=%d\n", target_rejected);
	printf("private_hidden=%d\n", private_hidden);
	printf("move_blocked=%d\n", move_blocked);
	printf("lock_stable=%d\n", lock_stable);

	munmap(device, len);
	close(fd);
	munmap(bound, len);
	munmap(ordinary, len);
	return 0;
}
