// SPDX-License-Identifier: GPL-2.0
/*
 * Helper for the private-node selftests.
 *
 * Driven by private_node_*.sh KTAP scripts, which own provisioning and verdicts
 * This tool performs an mmap/fault/policy ops and reports facts, exiting 0
 * (KSFT_SKIP on a setup error the script should treat as a skip).
 *
 * A few verbs (daxswap) fold a self-contained pass/fail into the exit status
 * because the verdict needs in-process counter state.
 *
 * We use raw syscalls to avoid any quirks in things like numactl (like
 * checking has_memory and refusing to call the syscall if not present).
 *
 * the script maps the exit code to a KTAP result.
 *
 *   map      <daxdev> <MB> <nid>             fault MB, report node residency
 *   shared   <daxdev>                        probe that MAP_SHARED is rejected
 *   ltpin    <daxdev> <MB> <nid>             FOLL_LONGTERM pin, report result
 *   ltpinhold <daxdev> <MB> <nid> <hold_s>   FOLL_LONGTERM pin, hold (unplug probe)
 *   anon     <MB> [hold_s]                   anon alloc, hold (for numa_maps reads)
 *   churn    <MB> [secs]                     paced anon pressure to drive reclaim/OOM
 *   daxmap   <daxdev> <MB> <nid> [hold_s]    fault a dax mapping, report+hold
 *   daxmadv  <daxdev> <MB> <pageout|cold|free>  fault then madvise
 *   daxswap  <daxdev> <nid> <MB> [evict_MB]  swap round-trip (rc 0 pass/1 fail/2 skip)
 *   mbind    <nid> <MB>                      mbind(MPOL_BIND|STATIC) anon, report
 *   mbindns  <nid> <MB>                      mbind without STATIC_NODES
 *   mbindthp <nid> <MB> [hold_s]             mbind a THP range, hold
 *   mbindmask <MB> <nid>...                  mbind(MPOL_BIND) to a node mask
 *   collapse <nid> <MB>                      mbind base pages, MADV_COLLAPSE, report THP
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/types.h>
#include "../../../../mm/gup_test.h"

#define KSFT_SKIP 4
#define GUP_TEST_FILE "/sys/kernel/debug/gup_test"
#define MAXNODE 256

#ifndef MPOL_DEFAULT
#define MPOL_DEFAULT 0
#endif
#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif
#ifndef MPOL_PREFERRED
#define MPOL_PREFERRED 1
#endif
#ifndef MPOL_INTERLEAVE
#define MPOL_INTERLEAVE 3
#endif
#ifndef MPOL_PREFERRED_MANY
#define MPOL_PREFERRED_MANY 5
#endif
#ifndef MPOL_WEIGHTED_INTERLEAVE
#define MPOL_WEIGHTED_INTERLEAVE 6
#endif
#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT	(1 << 0)
#define MPOL_MF_MOVE	(1 << 1)
#endif
#ifndef MPOL_F_STATIC_NODES
#define MPOL_F_STATIC_NODES (1 << 15)
#endif
#ifndef MPOL_F_RELATIVE_NODES
#define MPOL_F_RELATIVE_NODES (1 << 14)
#endif
#ifndef MADV_COLD
#define MADV_COLD	20
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT	21
#endif
#ifndef MADV_FREE
#define MADV_FREE	8
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE	14
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE	15
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE	25
#endif
#define HP_SIZE		(2UL << 20)
#define DAXSWAP_MAGIC	0x5741504bUL	/* "SWAP" */

static long sys_mbind(void *addr, unsigned long len, int mode,
		      const unsigned long *nmask, unsigned long maxnode, unsigned int flags)
{
	return syscall(__NR_mbind, addr, len, mode, nmask, maxnode, flags);
}
static long sys_move_pages(int pid, unsigned long count, void **pages,
			   const int *nodes, int *status, int flags)
{
	return syscall(__NR_move_pages, pid, count, pages, nodes, status, flags);
}
static long sys_set_mempolicy(int mode, const unsigned long *nmask,
			      unsigned long maxnode)
{
	return syscall(__NR_set_mempolicy, mode, nmask, maxnode);
}
static long sys_set_mempolicy_home_node(unsigned long start, unsigned long len,
					unsigned long home_node, unsigned long flags)
{
	return syscall(__NR_set_mempolicy_home_node, start, len, home_node, flags);
}
static long sys_migrate_pages(int pid, unsigned long maxnode,
			      const unsigned long *old, const unsigned long *new)
{
	return syscall(__NR_migrate_pages, pid, maxnode, old, new);
}

static void set_bit_node(unsigned long *mask, int nid)
{
	memset(mask, 0, MAXNODE / 8);
	mask[nid / (8 * sizeof(long))] |= 1UL << (nid % (8 * sizeof(long)));
}

/* Node the page at @addr resides on, or <0 on error. */
static int page_node(void *addr)
{
	void *p = addr;
	int status = -1;

	if (sys_move_pages(0, 1, &p, NULL, &status, 0) != 0)
		return -errno;
	return status;
}

/* Read a named counter from /proc/vmstat (e.g. "pswpin"); -1 if not found. */
static long vmstat(const char *name)
{
	char line[256];
	long val = -1;
	size_t nl = strlen(name);
	FILE *f = fopen("/proc/vmstat", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, name, nl) && line[nl] == ' ') {
			val = atol(line + nl + 1);
			break;
		}
	fclose(f);
	return val;
}

/*
 * Read a named smaps field in kB (e.g. "AnonHugePages") for the mapping
 * containing @addr; -1 if not found.
 */
static long smaps_field(unsigned long addr, const char *field)
{
	char line[256];
	unsigned long start, end;
	size_t fl = strlen(field);
	int in = 0;
	long val = -1;
	FILE *f = fopen("/proc/self/smaps", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in = (addr >= start && addr < end);
			continue;
		}
		if (in && !strncmp(line, field, fl) && line[fl] == ':') {
			val = atol(line + fl + 1);
			break;
		}
	}
	fclose(f);
	return val;
}

/*
 * Sum, from /proc/self/numa_maps, the pages of the mapping at @addr that live
 * on node @nid, the total mapped pages, and the swapped-out pages.
 */
static void numa_residency(unsigned long addr, int nid, unsigned long *total,
			   unsigned long *on_nid, unsigned long *swap)
{
	char line[8192];
	FILE *f = fopen("/proc/self/numa_maps", "r");

	*total = *on_nid = 0;
	if (swap)
		*swap = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		unsigned long start;
		char *tok;

		if (sscanf(line, "%lx", &start) != 1 || start != addr)
			continue;
		for (tok = strtok(line, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
			int n;
			unsigned long pages;

			if (swap && !strncmp(tok, "swap=", 5))
				*swap = atol(tok + 5);
			else if (tok[0] == 'N' && sscanf(tok, "N%d=%lu", &n, &pages) == 2) {
				*total += pages;
				if (n == nid)
					*on_nid += pages;
			}
		}
		break;
	}
	fclose(f);
}

static int do_map(const char *dev, unsigned long mb, int nid)
{
	unsigned long len = mb << 20, total, on_nid;
	int fd = open(dev, O_RDWR);
	void *p;

	if (fd < 0) {
		fprintf(stderr, "open(%s): %m\n", dev);
		return KSFT_SKIP;
	}
	/* the private mapping rejects VM_SHARED; the mapping becomes ordinary anon memory. */
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mmap(%s): %m\n", dev);
		close(fd);
		return KSFT_SKIP;
	}
	memset(p, 1, len);			/* fault every page */
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("total_pages=%lu on_node%d=%lu\n", total, nid, on_nid);
	munmap(p, len);
	close(fd);
	return 0;
}

static int do_shared(const char *dev)
{
	int fd = open(dev, O_RDWR);
	void *p;

	if (fd < 0) {
		fprintf(stderr, "open(%s): %m\n", dev);
		return KSFT_SKIP;
	}
	p = mmap(NULL, 2UL << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	printf("shared_mmap=%s errno=%d\n",
	       p == MAP_FAILED ? "rejected" : "accepted",
	       p == MAP_FAILED ? errno : 0);
	if (p != MAP_FAILED)
		munmap(p, 2UL << 20);
	close(fd);
	return 0;
}

/*
 * Attempt a FOLL_PIN|FOLL_LONGTERM pin of a private-node mapping and report
 * whether it succeeded and where the folios ended up.  A private-node folio that
 * did not opt into NODE_MEMORY_CAP_LTPIN must fail the pin AND stay in place (it
 * is neither pinnable nor migratable); an opted-in node pins like ordinary memory.
 */
static int do_ltpin(const char *dev, unsigned long mb, int nid)
{
	unsigned long len = mb << 20, total, on_nid;
	struct pin_longterm_test t = { 0 };
	int fd, gup, pinned;
	void *p;

	gup = open(GUP_TEST_FILE, O_RDWR);
	if (gup < 0) {
		fprintf(stderr, "open(%s): %m (need CONFIG_GUP_TEST + debugfs)\n", GUP_TEST_FILE);
		return KSFT_SKIP;
	}
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %m\n", dev);
		return KSFT_SKIP;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mmap(%s): %m\n", dev);
		return KSFT_SKIP;
	}
	memset(p, 1, len);

	t.addr = (unsigned long)p;
	t.size = len;
	t.flags = 0;				/* slow-path read FOLL_LONGTERM pin */
	pinned = ioctl(gup, PIN_LONGTERM_TEST_START, &t) == 0;
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("pinned=%s total_pages=%lu on_node%d=%lu\n",
	       pinned ? "yes" : "no", total, nid, on_nid);
	if (pinned)
		ioctl(gup, PIN_LONGTERM_TEST_STOP);
	munmap(p, len);
	close(fd);
	close(gup);
	return 0;
}

/*
 * Like ltpin, but hold the FOLL_LONGTERM pin for @hold s (printing our pid) so
 * the caller can probe an operation a pin must block, e.g. hot-unplug.  A kill
 * mid-hold closes the gup_test fd, which drops the pin.
 */
static int do_ltpinhold(const char *dev, unsigned long mb, int nid, long hold)
{
	unsigned long len = mb << 20, total, on_nid;
	struct pin_longterm_test t = { 0 };
	int fd, gup, pinned;
	void *p;

	gup = open(GUP_TEST_FILE, O_RDWR);
	if (gup < 0) {
		fprintf(stderr, "open(%s): %m (need CONFIG_GUP_TEST + debugfs)\n", GUP_TEST_FILE);
		return KSFT_SKIP;
	}
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %m\n", dev);
		return KSFT_SKIP;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mmap(%s): %m\n", dev);
		return KSFT_SKIP;
	}
	memset(p, 1, len);

	t.addr = (unsigned long)p;
	t.size = len;
	t.flags = 0;
	pinned = ioctl(gup, PIN_LONGTERM_TEST_START, &t) == 0;
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("pinned=%s pid=%d total_pages=%lu on_node%d=%lu\n",
	       pinned ? "yes" : "no", getpid(), total, nid, on_nid);
	fflush(stdout);
	if (!pinned) {
		munmap(p, len);
		close(fd);
		close(gup);
		return KSFT_SKIP;
	}
	sleep(hold);				/* hold the pin while the caller acts */
	ioctl(gup, PIN_LONGTERM_TEST_STOP);
	munmap(p, len);
	close(fd);
	close(gup);
	return 0;
}

static int do_anon(long mb, long hold_s)
{
	size_t len = (size_t)mb << 20;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

	if (p == MAP_FAILED) {
		printf("anon: mmap(%ld MB) FAILED: %m\n", mb);
		return 1;
	}
	memset(p, 1, len);
	printf("anon: populated %ld MB (first page node=%d)\n", mb, page_node(p));
	fflush(stdout);
	sleep(hold_s);
	return 0;
}

/*
 * Paced pressure: mmap anon WITHOUT MAP_POPULATE and repeatedly walk it,
 * faulting pages gradually so kswapd can page out the working-set overflow to
 * swap instead of a MAP_POPULATE burst tripping the OOM killer.  Sizing @mb
 * above available DRAM forces sustained reclaim.
 */
static int do_churn(long mb, long secs)
{
	size_t len = (size_t)mb << 20;
	long ps = sysconf(_SC_PAGESIZE);
	size_t np = len / ps, i;
	time_t end;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) {
		printf("churn: mmap(%ld MB) FAILED: %m\n", mb);
		return 1;
	}
	printf("churn: %ld MB, %ld s, walking gradually to drive reclaim\n", mb, secs);
	fflush(stdout);
	end = time(NULL) + secs;
	do {
		for (i = 0; i < np; i++)
			*(volatile char *)(p + i * ps) = (char)i;
	} while (time(NULL) < end);
	return 0;
}

static int do_daxmap(const char *path, long mb, int nid, long hold)
{
	size_t len = (size_t)mb << 20;
	unsigned long total, on_nid;
	long ps = sysconf(_SC_PAGESIZE);
	int fd = open(path, O_RDWR);
	char *p;

	if (fd < 0) {
		fprintf(stderr, "daxmap: open(%s): %m\n", path);
		return KSFT_SKIP;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "daxmap: mmap(%ld MB): %m\n", mb);
		close(fd);
		return KSFT_SKIP;
	}
	for (size_t i = 0; i < len; i += ps)
		*(volatile char *)(p + i) = 1;
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("daxmap: total_pages=%lu on_node%d=%lu (first=%d)\n",
	       total, nid, on_nid, page_node(p));
	fflush(stdout);
	sleep(hold);
	munmap(p, len);
	close(fd);
	return 0;
}

/*
 * Like daxmap, but print our pid+addr and hold @hold s so the caller can read
 * /proc/<pid>/numa_maps across a hot-unplug of the node.  On a private node the
 * dax_file mmap_prepare hook installs an MPOL_F_PRIVATE bind with no USER_NUMA
 * opt-in, so this drives the driver-owned-bind scrub path.
 */
static int do_daxmaphold(const char *path, long mb, int nid, long hold)
{
	size_t len = (size_t)mb << 20;
	unsigned long total, on_nid;
	long ps = sysconf(_SC_PAGESIZE);
	int fd = open(path, O_RDWR);
	char *p;

	if (fd < 0) {
		fprintf(stderr, "daxmaphold: open(%s): %m\n", path);
		return KSFT_SKIP;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "daxmaphold: mmap(%ld MB): %m\n", mb);
		close(fd);
		return KSFT_SKIP;
	}
	for (size_t i = 0; i < len; i += ps)
		*(volatile char *)(p + i) = 1;
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("daxmaphold: pid=%d addr=0x%lx total_pages=%lu on_node%d=%lu (first=%d)\n",
	       getpid(), (unsigned long)p, total, nid, on_nid, page_node(p));
	fflush(stdout);
	sleep(hold);
	munmap(p, len);
	close(fd);
	return 0;
}

/*
 * Walk a dax mapping: fault @mb repeatedly for @secs.  Sustains pressure on the
 * private node so its reclaim/demotion path drains it (used to drive demotion
 * *out* of a private node that sits above DRAM in the tier order).
 */
static int do_daxchurn(const char *path, long mb, long secs)
{
	size_t len = (size_t)mb << 20, i;
	long ps = sysconf(_SC_PAGESIZE);
	time_t end;
	int fd = open(path, O_RDWR);
	char *p;

	if (fd < 0) {
		fprintf(stderr, "daxchurn: open(%s): %m\n", path);
		return KSFT_SKIP;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		fprintf(stderr, "daxchurn: mmap(%ld MB): %m\n", mb);
		return KSFT_SKIP;
	}
	printf("daxchurn: %ld MB, %ld s, walking to drive demotion\n", mb, secs);
	fflush(stdout);
	end = time(NULL) + secs;
	do {
		for (i = 0; i < len; i += ps)
			*(volatile char *)(p + i) = 1;
	} while (time(NULL) < end);
	munmap(p, len);
	return 0;
}

/* Fault a dax mapping, then madvise() it (pageout|cold|free). */
static int do_daxmadv(const char *path, long mb, const char *adv)
{
	size_t len = (size_t)mb << 20;
	long ps = sysconf(_SC_PAGESIZE);
	int advice, rc, fd = open(path, O_RDWR);
	char *p;

	if (!strcmp(adv, "pageout"))
		advice = MADV_PAGEOUT;
	else if (!strcmp(adv, "cold"))
		advice = MADV_COLD;
	else if (!strcmp(adv, "free"))
		advice = MADV_FREE;
	else {
		fprintf(stderr, "daxmadv: unknown advice '%s'\n", adv);
		return KSFT_SKIP;
	}
	if (fd < 0) {
		fprintf(stderr, "daxmadv: open(%s): %m\n", path);
		return KSFT_SKIP;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "daxmadv: mmap(%ld MB): %m\n", mb);
		return KSFT_SKIP;
	}
	for (size_t i = 0; i < len; i += ps)
		*(volatile char *)(p + i) = 1;
	printf("daxmadv: faulted %ld MB on node=%d\n", mb, page_node(p));
	errno = 0;
	rc = madvise(p, len, advice);
	printf("daxmadv: madvise(%s) rc=%d errno=%d (%s)\n",
	       adv, rc, errno, rc ? strerror(errno) : "ok");
	fflush(stdout);
	sleep(3);
	return 0;
}

/*
 * Pressure the private node by faulting a fresh @cap_mb private-node mapping of
 * @path, then unmap it.  Drops clean swap-cache folios off the node so a later
 * read of a paged-out region really swaps in.  Returns MB actually faulted.
 */
static long daxswap_evict_cache(const char *path, long cap_mb)
{
	long ps = sysconf(_SC_PAGESIZE);
	size_t len = (size_t)cap_mb << 20, done;
	int fd = open(path, O_RDWR);
	char *b;

	if (fd < 0)
		return 0;
	b = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	close(fd);
	if (b == MAP_FAILED)
		return 0;
	for (done = 0; done < len; done += ps)
		*(volatile char *)(b + done) = 1;
	munmap(b, len);
	return (long)(done >> 20);
}

/*
 * Swap round-trip for a private-node mapping.  Fault <MB> onto the private node,
 * stamp a per-page signature, page it out with MADV_PAGEOUT, evict the clean
 * swap-cache copies off the node, then read every page back -- which must really
 * swap in.  PASS (rc 0) requires: pswpin increased, the signature survived, and
 * the reallocated folios landed back on the private node.  rc 2 == inconclusive
 * (no swap-in happened / data lost: raise evict_mb) -> the script SKIPs.
 */
static int do_daxswap(const char *path, int nid, long mb, long evict_mb)
{
	long ps = sysconf(_SC_PAGESIZE);
	size_t len = (size_t)mb << 20, i;
	unsigned long resident, back, dummy;
	long bad = 0, zero = 0, swpin0, swpin1, ev;
	int rc, fd = open(path, O_RDWR);
	char *p;

	if (fd < 0) {
		fprintf(stderr, "daxswap: open(%s): %m\n", path);
		return KSFT_SKIP;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "daxswap: mmap(%ld MB): %m\n", mb);
		return KSFT_SKIP;
	}
	for (i = 0; i < len; i += ps)
		*(volatile unsigned long *)(p + i) = (i / ps) ^ DAXSWAP_MAGIC;
	numa_residency((unsigned long)p, nid, &dummy, &resident, NULL);
	if (resident == 0) {
		printf("daxswap: nothing landed on node%d\n", nid);
		return 1;
	}
	errno = 0;
	rc = madvise(p, len, MADV_PAGEOUT);
	printf("daxswap: madvise(PAGEOUT) rc=%d (%s)\n", rc, rc ? strerror(errno) : "ok");
	fflush(stdout);
	sleep(2);
	ev = daxswap_evict_cache(path, evict_mb);
	printf("daxswap: pressured node%d with %ld MB to drop swap cache\n", nid, ev);
	sleep(1);

	swpin0 = vmstat("pswpin");
	for (i = 0; i < len; i += ps) {
		unsigned long got = *(volatile unsigned long *)(p + i);

		if (got != ((i / ps) ^ DAXSWAP_MAGIC)) {
			bad++;
			if (got == 0)
				zero++;
		}
	}
	swpin1 = vmstat("pswpin");
	numa_residency((unsigned long)p, nid, &dummy, &back, NULL);
	printf("daxswap: %ld/%ld pages intact (%ld corrupt, %ld zeroed); pswpin +%ld; back_on_node%d=%lu/%lu\n",
	       (long)(len / ps) - bad, (long)(len / ps), bad, zero,
	       swpin1 - swpin0, nid, back, resident);
	fflush(stdout);

	if (swpin1 - swpin0 <= 0 || bad)
		return 2;			/* inconclusive -> script SKIPs */
	return back >= resident ? 0 : 1;
}

/* mbind a FRESH anon range onto @nid, fault it, report landing, hold @hold s. */
static int do_mbind_flags(int nid, long mb, long hold, int mode_flags, const char *tag)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))], total, on_nid;
	long ps = sysconf(_SC_PAGESIZE), npg = ((long)mb << 20) / ps, rc;
	size_t len = (size_t)mb << 20;
	char *p;

	set_bit_node(mask, nid);
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "%s: mmap: %m\n", tag);
		return KSFT_SKIP;
	}
	errno = 0;
	rc = sys_mbind(p, len, MPOL_BIND | mode_flags, mask, MAXNODE, 0);
	printf("%s: mbind(MPOL_BIND%s, node %d) rc=%ld errno=%d (%s)\n",
	       tag, (mode_flags & MPOL_F_STATIC_NODES) ? "|STATIC" : "", nid, rc,
	       errno, rc ? strerror(errno) : "ok");
	if (rc) {
		munmap(p, len);
		return 0;			/* rejected: script decides if expected */
	}
	for (long i = 0; i < npg; i++)
		p[i * ps] = 1;
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("%s: faulted %ld pages -> on_node%d=%lu total=%lu\n",
	       tag, npg, nid, on_nid, total);
	fflush(stdout);
	if (hold)
		sleep(hold);		/* keep resident so the script can read stats */
	munmap(p, len);
	return 0;
}

/* mbind a 2MB-aligned, MADV_HUGEPAGE anon range onto @nid (PMD THPs), hold. */
static int do_mbindthp(int nid, long mb, long hold)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))], total, on_nid;
	size_t len = (size_t)mb << 20;
	long rc, touched = 0;
	char *base, *p;

	set_bit_node(mask, nid);
	base = mmap(NULL, len + HP_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		fprintf(stderr, "mbindthp: mmap: %m\n");
		return KSFT_SKIP;
	}
	p = (char *)(((unsigned long)base + HP_SIZE - 1) & ~(HP_SIZE - 1));
	if (madvise(p, len, MADV_HUGEPAGE))
		fprintf(stderr, "mbindthp: madvise(HUGEPAGE): %m\n");
	errno = 0;
	rc = sys_mbind(p, len, MPOL_BIND | MPOL_F_STATIC_NODES, mask, MAXNODE, 0);
	printf("mbindthp: mbind(node %d) rc=%ld errno=%d (%s)\n",
	       nid, rc, errno, rc ? strerror(errno) : "ok");
	if (rc)
		return 0;
	for (size_t off = 0; off < len; off += HP_SIZE) {
		p[off] = 1;
		touched += HP_SIZE / 4096;
	}
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("mbindthp: faulted %zu MB -> on_node%d=%lu total=%lu (off=%lu)\n",
	       len >> 20, nid, on_nid, total, total - on_nid);
	fflush(stdout);
	sleep(hold);
	return 0;
}

/*
 * Bind a fresh 2MB-aligned anon range to @nid, fault it as BASE pages
 * (MADV_NOHUGEPAGE), then MADV_COLLAPSE it.  Reports whether a THP formed
 * (AnonHugePages) and where it landed.  Exercises khugepaged-style collapse
 * onto an opted private node: it should succeed only where the node permits it
 * (CAP_RECLAIM), and the collapsed THP should sit on @nid.
 */
static int do_collapse(int nid, long mb)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))], total, on_nid;
	size_t len = (size_t)mb << 20;
	long rc, ahp_pre, ahp_post, coll_pre, coll_post;
	char *base, *p;

	set_bit_node(mask, nid);
	base = mmap(NULL, len + HP_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		fprintf(stderr, "collapse: mmap: %m\n");
		return KSFT_SKIP;
	}
	p = (char *)(((unsigned long)base + HP_SIZE - 1) & ~(HP_SIZE - 1));

	/* Force plain base pages on fault: no fault-time THP. */
	if (madvise(p, len, MADV_NOHUGEPAGE))
		fprintf(stderr, "collapse: madvise(NOHUGEPAGE): %m\n");

	errno = 0;
	rc = sys_mbind(p, len, MPOL_BIND | MPOL_F_STATIC_NODES, mask, MAXNODE, 0);
	printf("collapse: mbind(node %d) rc=%ld errno=%d (%s)\n",
	       nid, rc, errno, rc ? strerror(errno) : "ok");
	if (rc)
		return 0;

	for (size_t off = 0; off < len; off += 4096)
		p[off] = 1;
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	ahp_pre = smaps_field((unsigned long)p, "AnonHugePages");
	printf("collapse: pre  on_node%d=%lu total=%lu AnonHugePages=%ldkB\n",
	       nid, on_nid, total, ahp_pre);

	/* Make the range THP-eligible, then collapse synchronously. */
	if (madvise(p, len, MADV_HUGEPAGE))
		fprintf(stderr, "collapse: madvise(HUGEPAGE): %m\n");
	coll_pre = vmstat("thp_collapse_alloc");
	errno = 0;
	rc = madvise(p, len, MADV_COLLAPSE);
	coll_post = vmstat("thp_collapse_alloc");
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	ahp_post = smaps_field((unsigned long)p, "AnonHugePages");
	printf("collapse: MADV_COLLAPSE rc=%ld errno=%d (%s) thp_collapse_alloc+%ld\n",
	       rc, errno, rc ? strerror(errno) : "ok", coll_post - coll_pre);
	printf("collapse: post on_node%d=%lu total=%lu AnonHugePages=%ldkB\n",
	       nid, on_nid, total, ahp_post);
	fflush(stdout);
	return 0;
}

/*
 * Like do_collapse(), but instead of MADV_COLLAPSE (userspace) it leaves the
 * range as base pages tagged MADV_HUGEPAGE and waits for the khugepaged DAEMON
 * to collapse it -- so the in-kernel gate sees cc->is_khugepaged=1.
 */
static int do_khugecollapse(int nid, long mb, long secs)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))], total, on_nid;
	size_t len = (size_t)mb << 20;
	long rc, ahp = 0;
	char *base, *p;
	time_t end;

	set_bit_node(mask, nid);
	base = mmap(NULL, len + HP_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		fprintf(stderr, "khugecollapse: mmap: %m\n");
		return KSFT_SKIP;
	}
	p = (char *)(((unsigned long)base + HP_SIZE - 1) & ~(HP_SIZE - 1));

	if (madvise(p, len, MADV_NOHUGEPAGE))	/* base pages on fault */
		fprintf(stderr, "khugecollapse: madvise(NOHUGEPAGE): %m\n");

	errno = 0;
	rc = sys_mbind(p, len, MPOL_BIND | MPOL_F_STATIC_NODES, mask, MAXNODE, 0);
	printf("khugecollapse: mbind(node %d) rc=%ld (%s)\n",
	       nid, rc, rc ? strerror(errno) : "ok");
	if (rc)
		return 0;

	for (size_t off = 0; off < len; off += 4096)
		p[off] = 1;
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("khugecollapse: pre  on_node%d=%lu total=%lu AnonHugePages=%ldkB\n",
	       nid, on_nid, total, smaps_field((unsigned long)p, "AnonHugePages"));

	/* Re-enable THP for the range so khugepaged will scan + collapse it. */
	if (madvise(p, len, MADV_HUGEPAGE))
		fprintf(stderr, "khugecollapse: madvise(HUGEPAGE): %m\n");

	printf("khugecollapse: waiting up to %lds for khugepaged...\n", secs);
	fflush(stdout);
	end = time(NULL) + secs;
	do {
		sleep(1);
		ahp = smaps_field((unsigned long)p, "AnonHugePages");
	} while (ahp <= 0 && time(NULL) < end);

	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("khugecollapse: post on_node%d=%lu total=%lu AnonHugePages=%ldkB\n",
	       nid, on_nid, total, ahp);
	fflush(stdout);
	return 0;
}

/* mbind(MPOL_BIND) a fresh anon range to a multi-node mask, fault, report. */
static int do_mbindmask(long mb, int nnids, char **nidv)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))], total, on_nid;
	long ps = sysconf(_SC_PAGESIZE), npg = ((long)mb << 20) / ps, rc;
	size_t len = (size_t)mb << 20;
	char list[128] = "";
	int i, nid0 = atoi(nidv[0]);
	char *p;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mbindmask: mmap: %m\n");
		return KSFT_SKIP;
	}
	memset(mask, 0, MAXNODE / 8);
	for (i = 0; i < nnids; i++) {
		int nid = atoi(nidv[i]);

		mask[nid / (8 * sizeof(long))] |= 1UL << (nid % (8 * sizeof(long)));
		snprintf(list + strlen(list), sizeof(list) - strlen(list),
			 "%s%d", i ? "," : "", nid);
	}
	errno = 0;
	rc = sys_mbind(p, len, MPOL_BIND, mask, MAXNODE, 0);
	printf("mbindmask: nodes={%s} rc=%ld errno=%d (%s)\n",
	       list, rc, errno, rc ? strerror(errno) : "ok");
	if (!rc) {
		for (long j = 0; j < npg; j++)
			p[j * ps] = 1;
		numa_residency((unsigned long)p, nid0, &total, &on_nid, NULL);
		printf("mbindmask: faulted %ld pages -> on_node%d=%lu total=%lu\n",
		       npg, nid0, on_nid, total);
	}
	munmap(p, len);
	return 0;
}

/*
 * Probe whether set_mempolicy(MPOL_BIND, {nid}) is honored for a private node
 * (CAP_USER_NUMA) -- report the rc only and immediately reset to the default
 * policy.  We deliberately do NOT fault under it: a *process-wide* strict bind
 * to a ZONE_MOVABLE private node would force even unmovable allocations (page
 * tables) onto a movable zone with no fallback and wedge the process.  Per-VMA
 * placement is exercised by the mbind test; here we only validate the gate.
 */
static int do_setmempol(int nid, long mb)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))];
	long rc;

	(void)mb;
	set_bit_node(mask, nid);
	errno = 0;
	rc = sys_set_mempolicy(MPOL_BIND, mask, MAXNODE);
	printf("setmempol: set_mempolicy(MPOL_BIND, node %d) rc=%ld errno=%d (%s)\n",
	       nid, rc, errno, rc ? strerror(errno) : "ok");
	if (!rc)
		sys_set_mempolicy(MPOL_DEFAULT, NULL, 0);	/* don't taint the process */
	return 0;
}

static int mpol_mode_from_str(const char *s)
{
	if (!strcmp(s, "bind"))		return MPOL_BIND;
	if (!strcmp(s, "preferred"))	return MPOL_PREFERRED;
	if (!strcmp(s, "prefmany"))	return MPOL_PREFERRED_MANY;
	if (!strcmp(s, "interleave"))	return MPOL_INTERLEAVE;
	if (!strcmp(s, "weighted"))	return MPOL_WEIGHTED_INTERLEAVE;
	return -1;
}

/*
 * mpolplace <mode> <nid> <MB>: set_mempolicy(mode, {nid}) then, on success,
 * fault <MB> of anon and dump per-node residency from numa_maps.  Lets a test
 * assert that private placement is BIND-only (non-bind -> EINVAL) and that a
 * private BIND lands on <nid> and on no other node.
 */
static int do_mpolplace(const char *modestr, int nid, long mb)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))];
	unsigned long len = mb << 20, addr, total, on_nid, swap;
	int mode = mpol_mode_from_str(modestr);
	char line[8192];
	FILE *f;
	void *p;
	long rc;

	if (mode < 0) {
		printf("mpolplace: bad mode '%s'\n", modestr);
		return 2;
	}
	memset(mask, 0, sizeof(mask));
	set_bit_node(mask, nid);
	errno = 0;
	rc = sys_set_mempolicy(mode, mask, MAXNODE);
	printf("mpolplace: set_mempolicy(%s, node %d) rc=%ld errno=%d (%s)\n",
	       modestr, nid, rc, errno, rc ? strerror(errno) : "ok");
	if (rc)
		return 0;	/* rejected; caller asserts the errno line */

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		sys_set_mempolicy(MPOL_DEFAULT, NULL, 0);
		return 1;
	}
	memset(p, 1, len);		/* fault in under the policy */
	addr = (unsigned long)p;
	numa_residency(addr, nid, &total, &on_nid, &swap);
	printf("mpolplace: resident total=%lu on_node%d=%lu\n", total, nid, on_nid);
	f = fopen("/proc/self/numa_maps", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			unsigned long start;
			char *tok;

			if (sscanf(line, "%lx", &start) != 1 || start != addr)
				continue;
			printf("mpolplace: numa_maps:");
			for (tok = strtok(line, " \t\n"); tok; tok = strtok(NULL, " \t\n"))
				if (tok[0] == 'N' && strchr(tok, '='))
					printf(" %s", tok);
			printf("\n");
			break;
		}
		fclose(f);
	}
	munmap(p, len);
	sys_set_mempolicy(MPOL_DEFAULT, NULL, 0);
	return 0;
}

/*
 * Deliberately wedge-prone probe: process-wide set_mempolicy(MPOL_BIND, {nid})
 * then fault anon.  If @nid is movable-only, even the fault's page-table
 * allocations are forced onto a movable zone with no fallback.  Prints
 * "bindfault: done" only if it completes; the caller watchdogs for a wedge.
 * With @hold > 0 it keeps the mapping (and its page tables) resident and prints
 * its pid, so the caller can read the node's PageTables and probe hot-unplug.
 */
static int do_bindfault(int nid, long mb, long hold)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))], total, on_nid;
	long ps = sysconf(_SC_PAGESIZE), npg = ((long)mb << 20) / ps;
	size_t len = (size_t)mb << 20;
	char *p;

	set_bit_node(mask, nid);
	if (sys_set_mempolicy(MPOL_BIND, mask, MAXNODE)) {
		printf("bindfault: set_mempolicy(node %d) rc=-1 errno=%d (%s)\n",
		       nid, errno, strerror(errno));
		return 0;
	}
	printf("bindfault: bound MPOL_BIND {%d}, faulting %ld MB...\n", nid, mb);
	fflush(stdout);
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		printf("bindfault: mmap: %m\n");
		return 0;
	}
	for (long i = 0; i < npg; i++)
		p[i * ps] = 1;			/* may wedge here on a movable-only bind */
	numa_residency((unsigned long)p, nid, &total, &on_nid, NULL);
	printf("bindfault: done pid=%d -> on_node%d=%lu total=%lu\n",
	       getpid(), nid, on_nid, total);
	fflush(stdout);
	if (hold)
		sleep(hold);		/* keep page tables resident for the caller */
	sys_set_mempolicy(MPOL_DEFAULT, NULL, 0);
	munmap(p, len);
	return 0;
}

/*
 * mbind a range to a base (real) node, then set its home node.  home_node is
 * only a preferred-nid hint -- placement stays governed by the bind nodemask
 * ({base} here) -- so even a private home node must not be CAP-gated and must
 * not pull the allocation onto itself.  Report rc, and on success fault and
 * report how many pages landed on the home node (expected 0 when home != base).
 */
static int do_sethome(int home_nid, int base_nid, long mb)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))], total, on_home;
	long ps = sysconf(_SC_PAGESIZE), npg = ((long)mb << 20) / ps;
	size_t len = (size_t)mb << 20;
	long rc;
	char *p;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "sethome: mmap: %m\n");
		return KSFT_SKIP;
	}
	set_bit_node(mask, base_nid);
	if (sys_mbind(p, len, MPOL_BIND | MPOL_F_STATIC_NODES, mask, MAXNODE, 0)) {
		fprintf(stderr, "sethome: base mbind(node %d): %m\n", base_nid);
		munmap(p, len);
		return KSFT_SKIP;
	}
	errno = 0;
	rc = sys_set_mempolicy_home_node((unsigned long)p, len, home_nid, 0);
	if (rc) {
		printf("sethome: set_mempolicy_home_node(home %d, base %d) rc=%ld errno=%d (%s)\n",
		       home_nid, base_nid, rc, errno, strerror(errno));
		munmap(p, len);
		return 0;
	}
	for (long i = 0; i < npg; i++)
		p[i * ps] = 1;
	numa_residency((unsigned long)p, home_nid, &total, &on_home, NULL);
	printf("sethome: set_mempolicy_home_node(home %d, base %d) rc=0 ok; faulted %lu pages -> on_home%d=%lu\n",
	       home_nid, base_nid, total, home_nid, on_home);
	munmap(p, len);
	return 0;
}

/*
 * move_pages() a single private-node folio (faulted from @path) toward @target.
 * Reports the resulting per-page status: a private folio is migratable only if
 * its node allows mempolicy placement (CAP_USER_NUMA) -- otherwise -ENOENT.
 */
static int do_movepages(const char *path, int target_nid)
{
	long ps = sysconf(_SC_PAGESIZE);
	int fd = open(path, O_RDWR);
	void *pages[1];
	int nodes[1] = { target_nid };
	int status[1] = { 0x7fffffff };
	char *p;

	if (fd < 0) {
		fprintf(stderr, "movepages: open(%s): %m\n", path);
		return KSFT_SKIP;
	}
	p = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		fprintf(stderr, "movepages: mmap: %m\n");
		return KSFT_SKIP;
	}
	*(volatile char *)p = 1;			/* fault onto the private node */
	pages[0] = p;
	errno = 0;
	if (sys_move_pages(0, 1, pages, nodes, status, MPOL_MF_MOVE) != 0) {
		printf("movepages: src node%d -> target %d: move_pages rc=-1 errno=%d (%s)\n",
		       page_node(p), target_nid, errno, strerror(errno));
		munmap(p, ps);
		return 0;
	}
	printf("movepages: src -> target %d: status=%d (%s); now on node%d\n",
	       target_nid, status[0],
	       status[0] < 0 ? strerror(-status[0]) : "moved", page_node(p));
	munmap(p, ps);
	return 0;
}

/*
 * move_pages() a normal (DRAM) anon page TO @target.  A private node is a valid
 * move_pages() target only when it allows mempolicy placement (CAP_USER_NUMA);
 * otherwise the syscall fails -ENODEV (private nodes are not N_MEMORY).
 */
static int do_movepagesto(int target_nid)
{
	long ps = sysconf(_SC_PAGESIZE);
	void *pages[1];
	int nodes[1] = { target_nid };
	int status[1] = { 0x7fffffff };
	char *p;

	p = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "movepagesto: mmap: %m\n");
		return KSFT_SKIP;
	}
	*(volatile char *)p = 1;			/* fault onto a normal node */
	pages[0] = p;
	errno = 0;
	if (sys_move_pages(0, 1, pages, nodes, status, MPOL_MF_MOVE) != 0) {
		printf("movepagesto: -> target %d: move_pages rc=-1 errno=%d (%s)\n",
		       target_nid, errno, strerror(errno));
		munmap(p, ps);
		return 0;
	}
	printf("movepagesto: -> target %d: status=%d (%s); now on node%d\n",
	       target_nid, status[0],
	       status[0] < 0 ? strerror(-status[0]) : "moved", page_node(p));
	munmap(p, ps);
	return 0;
}

/*
 * mbind a fresh anon VMA to {nid} and hold, so a driving script can watch
 * /proc/<pid>/numa_maps across cpuset.mems / hotplug changes.  mode:
 *   none          no policy (observe cgroup-v2 cpuset auto-migration)
 *   bind          MPOL_BIND (default remap: pol->nodes remapped positionally
 *                 on a cpuset.mems change)
 *   bindstatic    MPOL_BIND | MPOL_F_STATIC_NODES (absolute, never remapped)
 *   bindrelative  MPOL_BIND | MPOL_F_RELATIVE_NODES (the nid is a POSITION,
 *                 folded onto the userland-NUMA nodes of the current cpuset)
 * Prints "pid=.. addr=0x.." (the per-VMA policy never wedges page-table allocs).
 */
static int do_mbindhold(const char *mode, int nid, long mb, long hold)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))];
	long ps = sysconf(_SC_PAGESIZE), npg = ((long)mb << 20) / ps, rc = 0;
	size_t len = (size_t)mb << 20;
	int flags = 0;
	char *p;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mbindhold: mmap: %m\n");
		return KSFT_SKIP;
	}
	if (strcmp(mode, "none")) {
		if (!strcmp(mode, "bindstatic"))
			flags = MPOL_F_STATIC_NODES;
		else if (!strcmp(mode, "bindrelative"))
			flags = MPOL_F_RELATIVE_NODES;
		else if (strcmp(mode, "bind")) {
			fprintf(stderr, "mbindhold: bad mode '%s'\n", mode);
			return KSFT_SKIP;
		}
		set_bit_node(mask, nid);
		errno = 0;
		rc = sys_mbind(p, len, MPOL_BIND | flags, mask, MAXNODE, 0);
		if (rc) {
			printf("mbindhold: pid=%d mbind(%s,{%d}) rc=%ld errno=%d (%s)\n",
			       getpid(), mode, nid, rc, errno, strerror(errno));
			fflush(stdout);
			sleep(hold);
			munmap(p, len);
			return 0;
		}
	}
	for (long i = 0; i < npg; i++)
		p[i * ps] = 1;
	printf("mbindhold: pid=%d addr=0x%lx mode=%s nid=%d mb=%ld faulted first=node%d\n",
	       getpid(), (unsigned long)p, mode, nid, mb, page_node(p));
	fflush(stdout);
	sleep(hold);
	munmap(p, len);
	return 0;
}

/*
 * Two-phase bind to probe stale-bind reattachment across a node's offline/online.
 * mbind a fresh anon range to {nid} (non-static, so an opted private nid gets
 * MPOL_F_PRIVATE), fault the FIRST half (phase 1), print pid+addr, hold @hold s
 * (the caller offlines and re-onlines @nid), then fault the SECOND half (fresh
 * pages, phase 2) and report how many landed back on @nid.  on_node==0 means the
 * stale bind was scrubbed by the offline; >0 means it reattached.
 */
static int do_mbindrefault(int nid, long mb, long hold)
{
	unsigned long mask[MAXNODE / (8 * sizeof(long))];
	long ps = sysconf(_SC_PAGESIZE), on2 = 0, n2 = 0, rc;
	size_t len = (size_t)mb << 20, half = len / 2, i;
	char *p;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mbindrefault: mmap: %m\n");
		return KSFT_SKIP;
	}
	set_bit_node(mask, nid);
	errno = 0;
	rc = sys_mbind(p, len, MPOL_BIND, mask, MAXNODE, 0);
	if (rc) {
		printf("mbindrefault: mbind({%d}) rc=%ld errno=%d (%s)\n",
		       nid, rc, errno, strerror(errno));
		fflush(stdout);
		munmap(p, len);
		return 0;
	}
	for (i = 0; i < half; i += ps)
		p[i] = 1;			/* phase 1 */
	printf("mbindrefault: pid=%d addr=0x%lx phase1 first=node%d\n",
	       getpid(), (unsigned long)p, page_node(p));
	fflush(stdout);
	sleep(hold);				/* caller offlines + re-onlines @nid */
	for (i = half; i < len; i += ps)
		p[i] = 1;			/* phase 2: fresh pages */
	for (i = half; i < len; i += ps) {
		n2++;
		if (page_node(p + i) == nid)
			on2++;
	}
	printf("mbindrefault: phase2 on_node%d=%ld total2=%ld\n", nid, on2, n2);
	fflush(stdout);
	munmap(p, len);
	return 0;
}

/*
 * migrate_pages(2): mbind+fault @mb on @oldn, then migrate_pages(old={oldn},
 * new={newn}) and report residency.  Probes the CAP_USER_NUMA gate on both ends:
 * a private node is migratable to/from only when it opted into userspace
 * placement (otherwise off-private is queue-skipped and onto-private is refused).
 */
static int do_migratepages2(int oldn, int newn, long mb)
{
	unsigned long m[MAXNODE / (8 * sizeof(long))];
	unsigned long old[MAXNODE / (8 * sizeof(long))];
	unsigned long new[MAXNODE / (8 * sizeof(long))];
	unsigned long total, on_old, on_new;
	long ps = sysconf(_SC_PAGESIZE), npg = ((long)mb << 20) / ps, rc;
	size_t len = (size_t)mb << 20;
	char *p;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "migratepages2: mmap: %m\n");
		return KSFT_SKIP;
	}
	set_bit_node(m, oldn);
	if (sys_mbind(p, len, MPOL_BIND | MPOL_F_STATIC_NODES, m, MAXNODE, 0)) {
		fprintf(stderr, "migratepages2: mbind(old %d): %m\n", oldn);
		return KSFT_SKIP;
	}
	for (long i = 0; i < npg; i++)
		p[i * ps] = 1;
	numa_residency((unsigned long)p, oldn, &total, &on_old, NULL);
	/* drop the bind so migrate_pages is free to relocate */
	sys_set_mempolicy(MPOL_DEFAULT, NULL, 0);
	memset(old, 0, sizeof(old)); memset(new, 0, sizeof(new));
	old[oldn / (8 * sizeof(long))] |= 1UL << (oldn % (8 * sizeof(long)));
	new[newn / (8 * sizeof(long))] |= 1UL << (newn % (8 * sizeof(long)));
	errno = 0;
	rc = sys_migrate_pages(0, MAXNODE, old, new);
	numa_residency((unsigned long)p, newn, &total, &on_new, NULL);
	numa_residency((unsigned long)p, oldn, &total, &on_old, NULL);
	printf("migratepages2: %d->%d rc=%ld errno=%d (%s) on_old%d=%lu on_new%d=%lu total=%lu\n",
	       oldn, newn, rc, errno, rc < 0 ? strerror(errno) : "ok",
	       oldn, on_old, newn, on_new, total);
	munmap(p, len);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 5 && !strcmp(argv[1], "map"))
		return do_map(argv[2], strtoul(argv[3], NULL, 0), atoi(argv[4]));
	if (argc >= 3 && !strcmp(argv[1], "shared"))
		return do_shared(argv[2]);
	if (argc >= 5 && !strcmp(argv[1], "ltpin"))
		return do_ltpin(argv[2], strtoul(argv[3], NULL, 0), atoi(argv[4]));
	if (argc == 6 && !strcmp(argv[1], "ltpinhold"))
		return do_ltpinhold(argv[2], strtoul(argv[3], NULL, 0), atoi(argv[4]),
				    atol(argv[5]));
	if (argc >= 3 && !strcmp(argv[1], "anon"))
		return do_anon(atol(argv[2]), argc >= 4 ? atol(argv[3]) : 10);
	if (argc >= 3 && !strcmp(argv[1], "churn"))
		return do_churn(atol(argv[2]), argc >= 4 ? atol(argv[3]) : 12);
	if (argc >= 5 && !strcmp(argv[1], "daxmap"))
		return do_daxmap(argv[2], atol(argv[3]), atoi(argv[4]),
				 argc >= 6 ? atol(argv[5]) : 8);
	if (argc == 6 && !strcmp(argv[1], "daxmaphold"))
		return do_daxmaphold(argv[2], atol(argv[3]), atoi(argv[4]),
				     atol(argv[5]));
	if (argc >= 4 && !strcmp(argv[1], "daxchurn"))
		return do_daxchurn(argv[2], atol(argv[3]), argc >= 5 ? atol(argv[4]) : 30);
	if (argc == 5 && !strcmp(argv[1], "daxmadv"))
		return do_daxmadv(argv[2], atol(argv[3]), argv[4]);
	if (argc >= 5 && !strcmp(argv[1], "daxswap"))
		return do_daxswap(argv[2], atoi(argv[3]), atol(argv[4]),
				  argc >= 6 ? atol(argv[5]) : 1024);
	if (argc >= 4 && !strcmp(argv[1], "mbind"))
		return do_mbind_flags(atoi(argv[2]), atol(argv[3]),
				      argc >= 5 ? atol(argv[4]) : 0,
				      MPOL_F_STATIC_NODES, "mbind");
	if (argc >= 4 && !strcmp(argv[1], "mbindns"))
		return do_mbind_flags(atoi(argv[2]), atol(argv[3]),
				      argc >= 5 ? atol(argv[4]) : 0, 0, "mbindns");
	if (argc >= 4 && !strcmp(argv[1], "mbindthp"))
		return do_mbindthp(atoi(argv[2]), atol(argv[3]),
				   argc >= 5 ? atol(argv[4]) : 0);
	if (argc == 4 && !strcmp(argv[1], "collapse"))
		return do_collapse(atoi(argv[2]), atol(argv[3]));
	if (argc >= 4 && !strcmp(argv[1], "khugecollapse"))
		return do_khugecollapse(atoi(argv[2]), atol(argv[3]),
					argc >= 5 ? atol(argv[4]) : 20);
	if (argc >= 4 && !strcmp(argv[1], "mbindmask"))
		return do_mbindmask(atol(argv[2]), argc - 3, &argv[3]);
	if (argc == 4 && !strcmp(argv[1], "setmempol"))
		return do_setmempol(atoi(argv[2]), atol(argv[3]));
	if (argc == 5 && !strcmp(argv[1], "mpolplace"))
		return do_mpolplace(argv[2], atoi(argv[3]), atol(argv[4]));
	if (argc == 5 && !strcmp(argv[1], "sethome"))
		return do_sethome(atoi(argv[2]), atoi(argv[3]), atol(argv[4]));
	if (argc >= 4 && !strcmp(argv[1], "bindfault"))
		return do_bindfault(atoi(argv[2]), atol(argv[3]),
				    argc >= 5 ? atol(argv[4]) : 0);
	if (argc == 5 && !strcmp(argv[1], "mbindrefault"))
		return do_mbindrefault(atoi(argv[2]), atol(argv[3]), atol(argv[4]));
	if (argc == 6 && !strcmp(argv[1], "mbindhold"))
		return do_mbindhold(argv[2], atoi(argv[3]), atol(argv[4]),
				    atol(argv[5]));
	if (argc == 5 && !strcmp(argv[1], "migratepages2"))
		return do_migratepages2(atoi(argv[2]), atoi(argv[3]), atol(argv[4]));
	if (argc == 4 && !strcmp(argv[1], "movepages"))
		return do_movepages(argv[2], atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "movepagesto"))
		return do_movepagesto(atoi(argv[2]));

	fprintf(stderr,
		"usage: %s map <daxdev> <MB> <nid> | shared <daxdev> | ltpin <daxdev> <MB> <nid> |\n"
		"       anon <MB> [hold] | churn <MB> [secs] | daxmap <daxdev> <MB> <nid> [hold] |\n"
		"       daxmaphold <daxdev> <MB> <nid> <hold_s> |\n"
		"       daxchurn <daxdev> <MB> [secs] | daxmadv <daxdev> <MB> <pageout|cold|free> |\n"
		"       daxswap <daxdev> <nid> <MB> [evict_MB] |\n"
		"       mbind <nid> <MB> [hold] | mbindns <nid> <MB> [hold] |\n"
		"       mbindthp <nid> <MB> [hold] | mbindmask <MB> <nid>... | collapse <nid> <MB> |\n"
		"       setmempol <nid> <MB> | sethome <home_nid> <base_nid> <MB> |\n"
		"       bindfault <nid> <MB> [hold_s] |\n"
		"       ltpinhold <daxdev> <MB> <nid> <hold_s> |\n"
		"       mbindhold <none|bind|bindstatic|bindrelative> <nid> <MB> <hold_s> |\n"
		"       mbindrefault <nid> <MB> <hold_s> |\n"
		"       migratepages2 <old_nid> <new_nid> <MB> |\n"
		"       movepages <daxdev> <target_nid> | movepagesto <target_nid>\n",
		argv[0]);
	return KSFT_SKIP;
}
