// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cram_demote.h"

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define THP_SIZE (2UL << 20)
#define PATTERN 0xab

static long page_size;

static long pages_on_node(void *addr, int nid)
{
	char line[4096], key[32], token[32];
	unsigned long target = (unsigned long)addr, start, end, vma_start = 0;
	FILE *file = fopen("/proc/self/maps", "r");
	long pages = -1;

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2 &&
		    start <= target && target < end) {
			vma_start = start;
			break;
		}
	}
	fclose(file);
	if (!vma_start)
		return -1;

	file = fopen("/proc/self/numa_maps", "r");
	if (!file)
		return -1;
	snprintf(key, sizeof(key), "%lx", vma_start);
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

static unsigned long pfn_of(void *addr)
{
	uint64_t entry = 0;
	off_t off = ((uintptr_t)addr / page_size) * sizeof(entry);
	int fd = open("/proc/self/pagemap", O_RDONLY);

	if (fd < 0)
		return 0;
	if (pread(fd, &entry, sizeof(entry), off) != sizeof(entry))
		entry = 0;
	close(fd);
	if (!(entry & (1ULL << 63)))
		return 0;
	return entry & ((1ULL << 55) - 1);
}

static long anon_huge_kb(void *addr, size_t len)
{
	unsigned long lo = (unsigned long)addr, hi = lo + len;
	unsigned long start, end;
	char line[256];
	long total = 0, value;
	int inside = 0;
	FILE *file;

	file = fopen("/proc/self/smaps", "r");
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2)
			inside = start < hi && end > lo;
		else if (inside && sscanf(line, "AnonHugePages: %ld", &value) == 1)
			total += value;
	}
	fclose(file);
	return total;
}

static void *aligned_mapping(size_t len)
{
	unsigned long aligned, base;
	void *raw;

	raw = mmap(NULL, len + THP_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		return NULL;
	base = (unsigned long)raw;
	aligned = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
	if (aligned > base)
		munmap((void *)base, aligned - base);
	if (aligned + len < base + len + THP_SIZE)
		munmap((void *)(aligned + len), base + len + THP_SIZE - aligned - len);
	return (void *)aligned;
}

static int test_collapse(int nid)
{
	const size_t len = 2 * THP_SIZE;
	void *ptr = aligned_mapping(len);
	long before, after, huge;
	int ret;

	if (!ptr)
		return 2;
	if (madvise(ptr, len, MADV_NOHUGEPAGE)) {
		munmap(ptr, len);
		return 3;
	}
	memset(ptr, PATTERN, len);
	if (cram_pageout(ptr, len)) {
		munmap(ptr, len);
		return 2;
	}
	before = pages_on_node(ptr, nid);
	if (before <= 0) {
		munmap(ptr, len);
		return 3;
	}

	madvise(ptr, len, MADV_HUGEPAGE);
	errno = 0;
	ret = madvise(ptr, len, MADV_COLLAPSE);
	huge = anon_huge_kb(ptr, len);
	after = pages_on_node(ptr, nid);
	printf("collapse_excluded=%d collapse_stable=%d ret=%d errno=%d huge_kb=%ld\n",
	       ret != 0 && huge == 0, after == before, ret, errno, huge);
	munmap(ptr, len);
	return 0;
}

static long read_long(const char *path)
{
	char buf[64];
	int fd = open(path, O_RDONLY);
	ssize_t len;

	if (fd < 0)
		return -1;
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0)
		return -1;
	buf[len] = '\0';
	return atol(buf);
}

static int write_long(const char *path, long value)
{
	char buf[64];
	int fd = open(path, O_WRONLY);
	int len;

	if (fd < 0)
		return -1;
	len = snprintf(buf, sizeof(buf), "%ld", value);
	len = write(fd, buf, len) == len ? 0 : -1;
	close(fd);
	return len;
}

static int test_ksm(int nid)
{
	const char *run_path = "/sys/kernel/mm/ksm/run";
	const char *scan_path = "/sys/kernel/mm/ksm/pages_to_scan";
	const char *sleep_path = "/sys/kernel/mm/ksm/sleep_millisecs";
	const char *full_path = "/sys/kernel/mm/ksm/full_scans";
	const char *merge_path = "/sys/kernel/mm/ksm/merge_across_nodes";
	char *priv_a, *priv_b, *pub_a, *pub_b;
	long old_run, old_scan, old_sleep, old_merge, full;
	unsigned long priv_pa, priv_pb, pub_pa, pub_pb;
	int i, ret = 0;

	old_run = read_long(run_path);
	old_scan = read_long(scan_path);
	old_sleep = read_long(sleep_path);
	old_merge = read_long(merge_path);
	if (old_run < 0 || old_scan < 0 || old_sleep < 0)
		return 3;
	write_long(run_path, 0);

	priv_a = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	priv_b = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	pub_a = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	pub_b = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (priv_a == MAP_FAILED || priv_b == MAP_FAILED ||
	    pub_a == MAP_FAILED || pub_b == MAP_FAILED) {
		ret = 2;
		goto restore;
	}

	if (madvise(priv_a, page_size, MADV_MERGEABLE) ||
	    madvise(priv_b, page_size, MADV_MERGEABLE) ||
	    madvise(pub_a, page_size, MADV_MERGEABLE) ||
	    madvise(pub_b, page_size, MADV_MERGEABLE)) {
		ret = 3;
		goto unmap;
	}
	memset(priv_a, PATTERN, page_size);
	memset(priv_b, PATTERN, page_size);
	memset(pub_a, PATTERN, page_size);
	memset(pub_b, PATTERN, page_size);
	if (cram_pageout(priv_a, page_size) ||
	    cram_pageout(priv_b, page_size)) {
		ret = 3;
		goto unmap;
	}
	if (pages_on_node(priv_a, nid) <= 0 || pages_on_node(priv_b, nid) <= 0) {
		ret = 3;
		goto unmap;
	}

	full = read_long(full_path);
	write_long(scan_path, 1000000);
	write_long(sleep_path, 0);
	if (old_merge >= 0)
		write_long(merge_path, 1);
	write_long(run_path, 1);
	for (i = 0; i < 300 && read_long(full_path) < full + 3; i++)
		usleep(100000);
	if (i == 300) {
		ret = 3;
		goto unmap;
	}

	priv_pa = pfn_of(priv_a);
	priv_pb = pfn_of(priv_b);
	pub_pa = pfn_of(pub_a);
	pub_pb = pfn_of(pub_b);
	printf("private_unmerged=%d common_merged=%d\n",
	       priv_pa && priv_pb && priv_pa != priv_pb,
	       pub_pa && pub_pb && pub_pa == pub_pb);

unmap:
	munmap(priv_a, page_size);
	munmap(priv_b, page_size);
	munmap(pub_a, page_size);
	munmap(pub_b, page_size);
restore:
	write_long(run_path, 0);
	write_long(scan_path, old_scan);
	write_long(sleep_path, old_sleep);
	if (old_merge >= 0)
		write_long(merge_path, old_merge);
	write_long(run_path, old_run);
	return ret;
}

int main(int argc, char **argv)
{
	int nid;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <collapse|ksm> <cram-nid>\n", argv[0]);
		return 2;
	}
	page_size = sysconf(_SC_PAGESIZE);
	nid = atoi(argv[2]);
	if (!strcmp(argv[1], "collapse"))
		return test_collapse(nid);
	if (!strcmp(argv[1], "ksm"))
		return test_ksm(nid);
	return 2;
}
