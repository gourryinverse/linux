// SPDX-License-Identifier: GPL-2.0
/*
 * Exercise MM services that must not walk folios on private memory nodes.
 *
 * The test does not create memory or change node features.  Boot with at
 * least one memory-only NUMA node carrying, for example,
 *
 *   private_node=<nid>,user
 *
 * so userspace can place the test mappings there.  At least one common node
 * is needed for the control cases; the NUMA-balancing test needs two.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/mempolicy.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define NODE_BASE "/sys/devices/system/node"
#define KSM_BASE "/sys/kernel/mm/ksm"
#define DAMON_ADMIN "/sys/kernel/mm/damon/admin"
#define DAMON_KD DAMON_ADMIN "/kdamonds/0"
#define DAMON_CTX DAMON_KD "/contexts/0"
#define DAMON_SCHEME DAMON_CTX "/schemes/0"
#define THP_SIZE (2UL * 1024 * 1024)

enum test_status {
	TEST_PASS,
	TEST_FAIL,
	TEST_SKIP,
};

static size_t page_size;
static int private_node = -1;
static int common_node = -1;
static int local_common_node = -1;
static int remote_common_node = -1;
static int max_node_id = -1;

static int read_long(const char *path, long *value)
{
	FILE *f = fopen(path, "r");
	int ret;

	if (!f)
		return -1;
	ret = fscanf(f, "%ld", value) == 1 ? 0 : -1;
	fclose(f);
	return ret;
}

static int write_text(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	ssize_t len = strlen(value);
	int ret = 0;

	if (fd < 0)
		return -1;
	if (write(fd, value, len) != len)
		ret = -1;
	close(fd);
	return ret;
}

static int write_long(const char *path, unsigned long value)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%lu", value);
	return write_text(path, buf);
}

static bool nodelist_has(const char *name, int nid)
{
	char path[PATH_MAX], buf[4096], *p;
	long first, last;
	FILE *f;

	snprintf(path, sizeof(path), NODE_BASE "/%s", name);
	f = fopen(path, "r");
	if (!f)
		return false;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return false;
	}
	fclose(f);

	for (p = buf; *p;) {
		while (*p == ',' || *p == ' ' || *p == '\n')
			p++;
		if (!*p)
			break;
		first = strtol(p, &p, 10);
		last = first;
		if (*p == '-') {
			p++;
			last = strtol(p, &p, 10);
		}
		if (nid >= first && nid <= last)
			return true;
		while (*p && *p != ',')
			p++;
	}
	return false;
}

static int nodelist_max(const char *name)
{
	char path[PATH_MAX], buf[4096], *p;
	long first, last, max = -1;
	FILE *f;

	snprintf(path, sizeof(path), NODE_BASE "/%s", name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	for (p = buf; *p;) {
		while (*p == ',' || *p == ' ' || *p == '\n')
			p++;
		if (!*p)
			break;
		first = strtol(p, &p, 10);
		last = first;
		if (*p == '-') {
			p++;
			last = strtol(p, &p, 10);
		}
		if (last > max)
			max = last;
		while (*p && *p != ',')
			p++;
	}
	return max;
}

static bool node_is_private(int nid)
{
	return nodelist_has("has_memory", nid) &&
	       !nodelist_has("has_common_memory", nid);
}

static bool node_is_usernuma(int nid)
{
	return nodelist_has("has_user_memory", nid);
}

static bool node_has_cpus(int nid)
{
	return nodelist_has("has_cpu", nid);
}

static void find_nodes(void)
{
	int nid;

	for (nid = 0; nid <= max_node_id; nid++) {
		if (!nodelist_has("has_memory", nid))
			continue;
		if (node_is_private(nid)) {
			if (private_node < 0 && node_is_usernuma(nid))
				private_node = nid;
			continue;
		}
		if (common_node < 0)
			common_node = nid;
		if (local_common_node < 0 && node_has_cpus(nid)) {
			local_common_node = nid;
			continue;
		}
		if (remote_common_node < 0)
			remote_common_node = nid;
	}

	/* A memory-only common node seen before the CPU node can be the control. */
	if (remote_common_node < 0) {
		for (nid = 0; nid <= max_node_id; nid++) {
			if (nid != local_common_node &&
			    nodelist_has("has_memory", nid) &&
			    !node_is_private(nid)) {
				remote_common_node = nid;
				break;
			}
		}
	}
}

static int bind_range(void *addr, size_t len, int nid)
{
	unsigned long bits = sizeof(unsigned long) * 8;
	unsigned long maxnode = max_node_id + 1;
	unsigned long *mask;
	int ret = -1;

	mask = calloc((maxnode + bits - 1) / bits, sizeof(*mask));
	if (!mask)
		return -1;
	mask[nid / bits] |= 1UL << (nid % bits);
#ifdef SYS_mbind
	ret = syscall(SYS_mbind, addr, len, MPOL_BIND, mask, maxnode, 0);
#else
	errno = EOPNOTSUPP;
#endif
	free(mask);
	return ret;
}

static int default_range(void *addr, size_t len)
{
#ifdef SYS_mbind
	return syscall(SYS_mbind, addr, len, MPOL_DEFAULT, NULL, 0, 0);
#else
	errno = EOPNOTSUPP;
	return -1;
#endif
}

static long resident_on(void *addr, size_t len, int nid)
{
	unsigned long pages = len / page_size;
	unsigned long found = 0, i;
	int got;

	for (i = 0; i < pages; i++) {
#ifdef SYS_get_mempolicy
		if (!syscall(SYS_get_mempolicy, &got, NULL, 0,
			     addr + i * page_size, MPOL_F_NODE | MPOL_F_ADDR) &&
		    got == nid)
			found++;
#endif
	}
	return found;
}

static unsigned long pfn_of(void *addr)
{
	uint64_t entry;
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

static int wait_ksm_scans(unsigned long count)
{
	long start, now;
	int waited;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), KSM_BASE "/full_scans");
	if (read_long(path, &start))
		return -1;
	for (waited = 0; waited < 30; waited++) {
		usleep(200000);
		if (!read_long(path, &now) && now >= start + (long)count)
			return 0;
	}
	return -1;
}

static int arm_ksm_pair(int nid, int value, char **a, char **b)
{
	int i;

	for (i = 0; i < 2; i++) {
		char **p = i ? b : a;

		*p = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (*p == MAP_FAILED || bind_range(*p, page_size, nid) ||
		    madvise(*p, page_size, MADV_MERGEABLE))
			return -1;
		memset(*p, value, page_size);
		if (resident_on(*p, page_size, nid) != 1)
			return -1;
	}
	return 0;
}

static enum test_status test_ksm(void)
{
	char run_path[PATH_MAX], scan_path[PATH_MAX], sleep_path[PATH_MAX];
	char *pa = NULL, *pb = NULL, *xa = NULL, *xb = NULL;
	unsigned long ppa, ppb, pxa, pxb;
	long old_run, old_scan, old_sleep;
	enum test_status ret = TEST_SKIP;

	snprintf(run_path, sizeof(run_path), KSM_BASE "/run");
	snprintf(scan_path, sizeof(scan_path), KSM_BASE "/pages_to_scan");
	snprintf(sleep_path, sizeof(sleep_path), KSM_BASE "/sleep_millisecs");
	if (access(run_path, R_OK | W_OK) || read_long(run_path, &old_run) ||
	    read_long(scan_path, &old_scan) || read_long(sleep_path, &old_sleep)) {
		ksft_print_msg("KSM sysfs is unavailable\n");
		return TEST_SKIP;
	}

	write_long(run_path, 0);
	if (arm_ksm_pair(common_node, 'p', &pa, &pb) ||
	    arm_ksm_pair(private_node, 'x', &xa, &xb)) {
		ksft_print_msg("could not place KSM pages on the selected nodes\n");
		goto out;
	}
	if (write_long(scan_path, 100000) || write_long(sleep_path, 0) ||
	    write_long(run_path, 1) || wait_ksm_scans(3)) {
		ksft_print_msg("ksmd did not complete the required scans\n");
		goto out;
	}

	ppa = pfn_of(pa);
	ppb = pfn_of(pb);
	pxa = pfn_of(xa);
	pxb = pfn_of(xb);
	if (!ppa || !ppb || !pxa || !pxb) {
		ksft_print_msg("pagemap PFNs unavailable; CAP_SYS_ADMIN is required\n");
		goto out;
	}
	if (ppa != ppb) {
		ksft_print_msg("common control pages did not merge\n");
		goto out;
	}
	ret = pxa != pxb ? TEST_PASS : TEST_FAIL;
out:
	write_long(scan_path, old_scan);
	write_long(sleep_path, old_sleep);
	write_long(run_path, old_run);
	if (pa && pa != MAP_FAILED)
		munmap(pa, page_size);
	if (pb && pb != MAP_FAILED)
		munmap(pb, page_size);
	if (xa && xa != MAP_FAILED)
		munmap(xa, page_size);
	if (xb && xb != MAP_FAILED)
		munmap(xb, page_size);
	return ret;
}

static long anon_huge_kb(void *addr, size_t len)
{
	unsigned long lo = (unsigned long)addr, hi = lo + len;
	unsigned long start, end;
	char line[256];
	long total = 0, value;
	bool inside = false;
	FILE *f = fopen("/proc/self/smaps", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2)
			inside = start < hi && end > lo;
		else if (inside && sscanf(line, "AnonHugePages: %ld", &value) == 1)
			total += value;
	}
	fclose(f);
	return total;
}

static enum test_status test_collapse(void)
{
	size_t len = 2 * THP_SIZE, map_len = len + THP_SIZE;
	unsigned long pages = len / page_size;
	char *raw, *addr;
	long before, after, huge;
	int collapse_ret;

	if (access("/sys/kernel/mm/transparent_hugepage/enabled", F_OK))
		return TEST_SKIP;
	raw = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		return TEST_SKIP;
	addr = (void *)(((uintptr_t)raw + THP_SIZE - 1) & ~(THP_SIZE - 1));
	if (bind_range(addr, len, private_node)) {
		munmap(raw, map_len);
		return TEST_SKIP;
	}
	madvise(addr, len, MADV_NOHUGEPAGE);
	memset(addr, 1, len);
	default_range(addr, len);
	before = resident_on(addr, len, private_node);
	if (before != (long)pages) {
		ksft_print_msg("only %ld/%lu collapse pages landed on private node %d\n",
			       before, pages, private_node);
		munmap(raw, map_len);
		return TEST_SKIP;
	}
	madvise(addr, len, MADV_HUGEPAGE);
	collapse_ret = madvise(addr, len, MADV_COLLAPSE);
	huge = anon_huge_kb(addr, len);
	after = resident_on(addr, len, private_node);
	munmap(raw, map_len);
	ksft_print_msg("MADV_COLLAPSE ret=%d errno=%d huge=%ld kB resident=%ld/%ld\n",
		       collapse_ret, collapse_ret ? errno : 0, huge, after, before);
	return huge == 0 && after == before ? TEST_PASS : TEST_FAIL;
}

static int pin_to_node(int nid)
{
	char path[PATH_MAX], buf[4096], *p;
	cpu_set_t cpus;
	long first, last, cpu;
	FILE *f;

	snprintf(path, sizeof(path), NODE_BASE "/node%d/cpulist", nid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	CPU_ZERO(&cpus);
	for (p = buf; *p;) {
		while (*p == ',' || *p == ' ' || *p == '\n')
			p++;
		if (!*p)
			break;
		first = strtol(p, &p, 10);
		last = first;
		if (*p == '-') {
			p++;
			last = strtol(p, &p, 10);
		}
		for (cpu = first; cpu <= last && cpu < CPU_SETSIZE; cpu++)
			CPU_SET(cpu, &cpus);
		while (*p && *p != ',')
			p++;
	}
	return sched_setaffinity(0, sizeof(cpus), &cpus);
}

static long hammer_node(int nid)
{
	const size_t len = 2048 * page_size;
	struct timespec now, end;
	unsigned long before, after, i;
	char *addr;

	addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED || bind_range((void *)addr, len, nid))
		return -1;
	for (i = 0; i < len; i += page_size)
		addr[i] = 1;
	if (default_range((void *)addr, len)) {
		munmap((void *)addr, len);
		return -1;
	}
	before = resident_on((void *)addr, len, nid);
	clock_gettime(CLOCK_MONOTONIC, &now);
	end = now;
	end.tv_sec += 10;
	do {
		for (i = 0; i < len; i += page_size)
			__atomic_fetch_add(&addr[i], 1, __ATOMIC_RELAXED);
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (now.tv_sec < end.tv_sec ||
		 (now.tv_sec == end.tv_sec && now.tv_nsec < end.tv_nsec));
	after = resident_on((void *)addr, len, nid);
	munmap((void *)addr, len);
	ksft_print_msg("node %d: %lu -> %lu pages resident\n", nid, before, after);
	return before ? (long)before - (long)after : -1;
}

static enum test_status test_numa_balancing(void)
{
	cpu_set_t saved_affinity;
	long old_setting, control, private;
	bool affinity_saved;

	if (local_common_node < 0 || remote_common_node < 0) {
		ksft_print_msg("NUMA balancing needs two common memory nodes\n");
		return TEST_SKIP;
	}
	if (read_long("/proc/sys/kernel/numa_balancing", &old_setting) ||
	    write_long("/proc/sys/kernel/numa_balancing", 1))
		return TEST_SKIP;
	affinity_saved = !sched_getaffinity(0, sizeof(saved_affinity),
					     &saved_affinity);
	if (pin_to_node(local_common_node)) {
		write_long("/proc/sys/kernel/numa_balancing", old_setting);
		return TEST_SKIP;
	}
	control = hammer_node(remote_common_node);
	if (control <= 0) {
		ksft_print_msg("common control did not demonstrate NUMA migration\n");
		if (affinity_saved)
			sched_setaffinity(0, sizeof(saved_affinity), &saved_affinity);
		write_long("/proc/sys/kernel/numa_balancing", old_setting);
		return TEST_SKIP;
	}
	private = hammer_node(private_node);
	if (affinity_saved)
		sched_setaffinity(0, sizeof(saved_affinity), &saved_affinity);
	write_long("/proc/sys/kernel/numa_balancing", old_setting);
	if (private < 0)
		return TEST_SKIP;
	return private == 0 ? TEST_PASS : TEST_FAIL;
}

static int damon_configure(pid_t pid)
{
	char value[64];
	const struct {
		const char *path;
		const char *value;
	} settings[] = {
		{ DAMON_ADMIN "/kdamonds/nr_kdamonds", "1" },
		{ DAMON_KD "/contexts/nr_contexts", "1" },
		{ DAMON_CTX "/operations", "vaddr" },
		{ DAMON_CTX "/monitoring_attrs/intervals/sample_us", "5000" },
		{ DAMON_CTX "/monitoring_attrs/intervals/aggr_us", "100000" },
		{ DAMON_CTX "/monitoring_attrs/intervals/update_us", "1000000" },
		{ DAMON_CTX "/monitoring_attrs/nr_regions/min", "10" },
		{ DAMON_CTX "/monitoring_attrs/nr_regions/max", "1000" },
		{ DAMON_CTX "/targets/nr_targets", "1" },
		{ DAMON_CTX "/schemes/nr_schemes", "1" },
		{ DAMON_SCHEME "/action", "stat" },
		{ DAMON_SCHEME "/access_pattern/sz/min", "0" },
		{ DAMON_SCHEME "/access_pattern/sz/max", "18446744073709551615" },
		{ DAMON_SCHEME "/access_pattern/nr_accesses/min", "1" },
		{ DAMON_SCHEME "/access_pattern/nr_accesses/max", "4294967295" },
		{ DAMON_SCHEME "/access_pattern/age/min", "0" },
		{ DAMON_SCHEME "/access_pattern/age/max", "4294967295" },
		{ DAMON_SCHEME "/quotas/ms", "0" },
		{ DAMON_SCHEME "/quotas/bytes", "0" },
		{ DAMON_SCHEME "/quotas/reset_interval_ms", "0" },
		{ DAMON_SCHEME "/watermarks/metric", "none" },
		{ DAMON_SCHEME "/watermarks/interval_us", "0" },
		{ DAMON_SCHEME "/watermarks/high", "0" },
		{ DAMON_SCHEME "/watermarks/mid", "0" },
		{ DAMON_SCHEME "/watermarks/low", "0" },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(settings); i++)
		if (write_text(settings[i].path, settings[i].value))
			return -1;
	snprintf(value, sizeof(value), "%d", pid);
	return write_text(DAMON_CTX "/targets/0/pid_target", value);
}

static unsigned long damon_count(unsigned long start, unsigned long end)
{
	unsigned long count = 0;
	int i;

	for (i = 0; ; i++) {
		char path[PATH_MAX];
		long rstart, rend, accesses;

		snprintf(path, sizeof(path), DAMON_SCHEME
			 "/tried_regions/%d/start", i);
		if (read_long(path, &rstart))
			break;
		snprintf(path, sizeof(path), DAMON_SCHEME
			 "/tried_regions/%d/end", i);
		if (read_long(path, &rend))
			break;
		snprintf(path, sizeof(path), DAMON_SCHEME
			 "/tried_regions/%d/nr_accesses", i);
		if (read_long(path, &accesses))
			break;
		if (accesses && (unsigned long)rstart < end &&
		    (unsigned long)rend > start)
			count++;
	}
	return count;
}

static void stop_damon(void)
{
	write_text(DAMON_KD "/state", "off");
	write_text(DAMON_ADMIN "/kdamonds/nr_kdamonds", "0");
}

static int damon_measure(int nid, unsigned long *count)
{
	const size_t len = 4096 * page_size;
	unsigned long *shared;
	pid_t child;
	int i, status, ret = -1;

	shared = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED)
		return -1;
	shared[0] = 0;
	shared[1] = 0;
	child = fork();
	if (child < 0)
		goto out;
	if (!child) {
		char *addr;
		unsigned long off;

		addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (addr == MAP_FAILED || bind_range((void *)addr, len, nid))
			_exit(2);
		for (off = 0; off < len; off += page_size)
			addr[off] = 1;
		if (resident_on((void *)addr, len, nid) != (long)(len / page_size))
			_exit(2);
		shared[0] = (unsigned long)addr;
		__atomic_store_n(&shared[1], 1, __ATOMIC_RELEASE);
		for (;;)
			for (off = 0; off < len; off += page_size)
				__atomic_fetch_add(&addr[off], 1,
						   __ATOMIC_RELAXED);
	}

	for (i = 0; i < 50; i++) {
		if (__atomic_load_n(&shared[1], __ATOMIC_ACQUIRE))
			break;
		if (waitpid(child, &status, WNOHANG) == child)
			goto child_done;
		usleep(100000);
	}
	if (!shared[1] || damon_configure(child) ||
	    write_text(DAMON_KD "/state", "on"))
		goto kill_child;
	sleep(3);
	if (write_text(DAMON_KD "/state", "update_schemes_tried_regions"))
		goto kill_child;
	*count = damon_count(shared[0], shared[0] + len);
	ret = 0;
kill_child:
	stop_damon();
	kill(child, SIGKILL);
	waitpid(child, &status, 0);
child_done:
out:
	munmap(shared, page_size);
	return ret;
}

static enum test_status test_damon(void)
{
	unsigned long common_count, private_count;
	long nr_kdamonds;

	if (access(DAMON_ADMIN, R_OK | W_OK) ||
	    read_long(DAMON_ADMIN "/kdamonds/nr_kdamonds", &nr_kdamonds))
		return TEST_SKIP;
	if (nr_kdamonds) {
		ksft_print_msg("DAMON is already configured; leaving it undisturbed\n");
		return TEST_SKIP;
	}
	if (damon_measure(common_node, &common_count) || !common_count) {
		ksft_print_msg("common control did not observe DAMON accesses\n");
		return TEST_SKIP;
	}
	if (damon_measure(private_node, &private_count))
		return TEST_SKIP;
	ksft_print_msg("DAMON regions: common=%lu private=%lu\n",
		       common_count, private_count);
	return private_count == 0 ? TEST_PASS : TEST_FAIL;
}

static void report(enum test_status status, const char *name)
{
	switch (status) {
	case TEST_PASS:
		ksft_test_result_pass("%s\n", name);
		break;
	case TEST_FAIL:
		ksft_test_result_fail("%s\n", name);
		break;
	case TEST_SKIP:
		ksft_test_result_skip("%s\n", name);
		break;
	}
}

int main(void)
{
	ksft_print_header();
	ksft_set_plan(4);
	page_size = getpagesize();

	if (geteuid()) {
		ksft_test_result_skip("KSM requires root\n");
		ksft_test_result_skip("MADV_COLLAPSE requires root\n");
		ksft_test_result_skip("NUMA balancing requires root\n");
		ksft_test_result_skip("DAMON requires root\n");
		ksft_finished();
	}
	max_node_id = nodelist_max("possible");
	if (max_node_id < 0 || access(NODE_BASE "/has_common_memory", R_OK) ||
	    access(NODE_BASE "/has_user_memory", R_OK)) {
		ksft_test_result_skip("private-node ABI is unavailable\n");
		ksft_test_result_skip("private-node ABI is unavailable\n");
		ksft_test_result_skip("private-node ABI is unavailable\n");
		ksft_test_result_skip("private-node ABI is unavailable\n");
		ksft_finished();
	}

	find_nodes();
	if (private_node < 0 || common_node < 0) {
		ksft_print_msg("need common and USER_NUMA private memory nodes\n");
		ksft_test_result_skip("KSM excludes private memory\n");
		ksft_test_result_skip("collapse excludes private memory\n");
		ksft_test_result_skip("automatic NUMA balancing excludes private memory\n");
		ksft_test_result_skip("DAMON excludes private memory\n");
		ksft_finished();
	}

	ksft_print_msg("common node %d, USER_NUMA private node %d\n",
		       common_node, private_node);
	report(test_ksm(), "KSM excludes private memory");
	report(test_collapse(), "collapse excludes private memory");
	report(test_numa_balancing(),
	       "automatic NUMA balancing excludes private memory");
	report(test_damon(), "DAMON excludes private memory");
	ksft_finished();
}
