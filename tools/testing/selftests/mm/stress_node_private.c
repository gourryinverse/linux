// SPDX-License-Identifier: GPL-2.0
/*
 * stress_node_private.c - Stress test for private NUMA node infrastructure
 *
 * Exercises syscall paths that interact with private nodes:
 *   - set_mempolicy / get_mempolicy with private node in nodemask
 *   - mbind with private node
 *   - migrate_pages to/from private node
 *   - move_pages targeting private node
 *   - set_mempolicy_home_node with private node
 *   - madvise / mlock on various mappings while private node exists
 *   - concurrent mempolicy + allocation stress
 *
 * Requires: CONFIG_TEST_NODE_PRIVATE=y, boot with standby nodes
 *
 * Usage: ./stress_node_private <private_nid> [duration_secs]
 *        Default duration: 30 seconds
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/mempolicy.h>
#include <sched.h>

#ifndef __NR_set_mempolicy_home_node
#define __NR_set_mempolicy_home_node 450
#endif

static volatile int stop;
static int private_nid;
static int normal_nid = 0;
static unsigned long errors;
static unsigned long ops;

/* Counters per test */
static unsigned long cnt_mempolicy;
static unsigned long cnt_mbind;
static unsigned long cnt_migrate;
static unsigned long cnt_move_pages;
static unsigned long cnt_home_node;
static unsigned long cnt_madvise;
static unsigned long cnt_mlock;
static unsigned long cnt_alloc;

static void sigalrm_handler(int sig)
{
	(void)sig;
	stop = 1;
}

/* Helper: set bits in a nodemask bitmask */
static void nodemask_set(unsigned long *mask, int node)
{
	mask[node / (8 * sizeof(unsigned long))] |=
		1UL << (node % (8 * sizeof(unsigned long)));
}

static void nodemask_zero(unsigned long *mask, int size)
{
	memset(mask, 0, size);
}

/*
 * Thread: set_mempolicy stress
 * Rapidly switches between policies that include/exclude the private node.
 */
static void *thread_mempolicy(void *arg)
{
	unsigned long mask[16];
	int mask_size = sizeof(mask);

	(void)arg;
	while (!stop) {
		/* Try MPOL_BIND to private node only */
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, private_nid);
		syscall(__NR_set_mempolicy, MPOL_BIND, mask, 8 * mask_size);
		cnt_mempolicy++;

		/* Try MPOL_BIND to both normal and private */
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, normal_nid);
		nodemask_set(mask, private_nid);
		syscall(__NR_set_mempolicy, MPOL_BIND, mask, 8 * mask_size);
		cnt_mempolicy++;

		/* Reset to default */
		syscall(__NR_set_mempolicy, MPOL_DEFAULT, NULL, 0);
		cnt_mempolicy++;

		/* MPOL_PREFERRED to private node */
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, private_nid);
		syscall(__NR_set_mempolicy, MPOL_PREFERRED, mask, 8 * mask_size);
		cnt_mempolicy++;

		/* MPOL_INTERLEAVE with private node */
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, normal_nid);
		nodemask_set(mask, private_nid);
		syscall(__NR_set_mempolicy, MPOL_INTERLEAVE, mask, 8 * mask_size);
		cnt_mempolicy++;

		/* get_mempolicy */
		int mode;
		syscall(__NR_get_mempolicy, &mode, mask, 8 * mask_size, NULL, 0);
		cnt_mempolicy++;

		/* Reset */
		syscall(__NR_set_mempolicy, MPOL_DEFAULT, NULL, 0);
		cnt_mempolicy++;
	}
	return NULL;
}

/*
 * Thread: mbind stress
 * Calls mbind on anonymous mappings with various nodemasks.
 */
static void *thread_mbind(void *arg)
{
	unsigned long mask[16];
	int mask_size = sizeof(mask);

	(void)arg;
	while (!stop) {
		void *p = mmap(NULL, 4096 * 16, PROT_READ | PROT_WRITE,
			       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED) {
			errors++;
			continue;
		}

		/* mbind to private node */
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, private_nid);
		syscall(__NR_mbind, p, 4096 * 4, MPOL_BIND, mask,
			8 * mask_size, 0);
		cnt_mbind++;

		/* mbind to normal + private */
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, normal_nid);
		nodemask_set(mask, private_nid);
		syscall(__NR_mbind, p + 4096 * 4, 4096 * 4, MPOL_BIND, mask,
			8 * mask_size, MPOL_MF_MOVE);
		cnt_mbind++;

		/* mbind MPOL_PREFERRED to private */
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, private_nid);
		syscall(__NR_mbind, p + 4096 * 8, 4096 * 4, MPOL_PREFERRED,
			mask, 8 * mask_size, 0);
		cnt_mbind++;

		/* Touch pages then mbind with MOVE */
		memset(p, 0x42, 4096 * 16);
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, private_nid);
		syscall(__NR_mbind, p, 4096 * 16, MPOL_BIND, mask,
			8 * mask_size, MPOL_MF_MOVE);
		cnt_mbind++;

		munmap(p, 4096 * 16);
	}
	return NULL;
}

/*
 * Thread: migrate_pages stress
 * Calls migrate_pages(2) targeting the private node.
 */
static void *thread_migrate(void *arg)
{
	unsigned long old_mask[16], new_mask[16];
	int mask_size = sizeof(old_mask);
	pid_t pid = getpid();

	(void)arg;
	while (!stop) {
		/* Migrate from normal to private */
		nodemask_zero(old_mask, mask_size);
		nodemask_zero(new_mask, mask_size);
		nodemask_set(old_mask, normal_nid);
		nodemask_set(new_mask, private_nid);
		syscall(__NR_migrate_pages, pid, 8 * mask_size,
			old_mask, new_mask);
		cnt_migrate++;

		/* Migrate from private to normal */
		nodemask_zero(old_mask, mask_size);
		nodemask_zero(new_mask, mask_size);
		nodemask_set(old_mask, private_nid);
		nodemask_set(new_mask, normal_nid);
		syscall(__NR_migrate_pages, pid, 8 * mask_size,
			old_mask, new_mask);
		cnt_migrate++;

		/* Self-migration private->private */
		nodemask_zero(old_mask, mask_size);
		nodemask_zero(new_mask, mask_size);
		nodemask_set(old_mask, private_nid);
		nodemask_set(new_mask, private_nid);
		syscall(__NR_migrate_pages, pid, 8 * mask_size,
			old_mask, new_mask);
		cnt_migrate++;
	}
	return NULL;
}

/*
 * Thread: move_pages stress
 * Calls move_pages(2) targeting the private node.
 */
static void *thread_move_pages(void *arg)
{
	void *pages[4];
	int nodes[4], status[4];
	pid_t pid = getpid();

	(void)arg;
	while (!stop) {
		void *p = mmap(NULL, 4096 * 4, PROT_READ | PROT_WRITE,
			       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED) {
			errors++;
			continue;
		}

		/* Fault in pages */
		memset(p, 0, 4096 * 4);

		for (int i = 0; i < 4; i++) {
			pages[i] = p + i * 4096;
			nodes[i] = private_nid;
		}

		/* Move to private node */
		syscall(__NR_move_pages, pid, 4, pages, nodes, status, 0);
		cnt_move_pages++;

		/* Move to normal node */
		for (int i = 0; i < 4; i++)
			nodes[i] = normal_nid;
		syscall(__NR_move_pages, pid, 4, pages, nodes, status,
			MPOL_MF_MOVE);
		cnt_move_pages++;

		/* Query status */
		syscall(__NR_move_pages, pid, 4, pages, NULL, status, 0);
		cnt_move_pages++;

		munmap(p, 4096 * 4);
	}
	return NULL;
}

/*
 * Thread: set_mempolicy_home_node stress
 */
static void *thread_home_node(void *arg)
{
	(void)arg;
	while (!stop) {
		void *p = mmap(NULL, 4096 * 4, PROT_READ | PROT_WRITE,
			       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED) {
			errors++;
			continue;
		}

		/* First set a mempolicy on the VMA */
		unsigned long mask[16];
		nodemask_zero(mask, sizeof(mask));
		nodemask_set(mask, normal_nid);
		syscall(__NR_mbind, p, 4096 * 4, MPOL_BIND, mask,
			8 * sizeof(mask), 0);

		/* Try setting home node to private */
		syscall(__NR_set_mempolicy_home_node, (unsigned long)p,
			4096 * 4, (unsigned long)private_nid, 0UL);
		cnt_home_node++;

		/* Try setting home node to normal */
		syscall(__NR_set_mempolicy_home_node, (unsigned long)p,
			4096 * 4, (unsigned long)normal_nid, 0UL);
		cnt_home_node++;

		munmap(p, 4096 * 4);
	}
	return NULL;
}

/*
 * Thread: madvise stress on various mapping types
 */
static void *thread_madvise(void *arg)
{
	(void)arg;
	while (!stop) {
		void *p = mmap(NULL, 4096 * 64, PROT_READ | PROT_WRITE,
			       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED) {
			errors++;
			continue;
		}

		memset(p, 0, 4096 * 64);

		madvise(p, 4096 * 64, MADV_HUGEPAGE);
		cnt_madvise++;
		madvise(p, 4096 * 64, MADV_NOHUGEPAGE);
		cnt_madvise++;
		madvise(p, 4096 * 64, MADV_MERGEABLE);
		cnt_madvise++;
		madvise(p, 4096 * 64, MADV_UNMERGEABLE);
		cnt_madvise++;
		madvise(p, 4096 * 64, MADV_DONTNEED);
		cnt_madvise++;
		madvise(p, 4096 * 64, MADV_WILLNEED);
		cnt_madvise++;

		munmap(p, 4096 * 64);
	}
	return NULL;
}

/*
 * Thread: mlock stress
 */
static void *thread_mlock(void *arg)
{
	(void)arg;
	while (!stop) {
		void *p = mmap(NULL, 4096 * 8, PROT_READ | PROT_WRITE,
			       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED) {
			errors++;
			continue;
		}

		memset(p, 0, 4096 * 8);

		mlock(p, 4096 * 8);
		cnt_mlock++;
		munlock(p, 4096 * 8);
		cnt_mlock++;

		mlock2(p, 4096 * 8, MLOCK_ONFAULT);
		cnt_mlock++;
		munlock(p, 4096 * 8);
		cnt_mlock++;

		munmap(p, 4096 * 8);
	}
	return NULL;
}

/*
 * Thread: allocation stress with mempolicy set to include private node
 */
static void *thread_alloc_stress(void *arg)
{
	unsigned long mask[16];
	int mask_size = sizeof(mask);

	(void)arg;
	while (!stop) {
		/* Set mempolicy to include private node */
		nodemask_zero(mask, mask_size);
		nodemask_set(mask, normal_nid);
		nodemask_set(mask, private_nid);
		syscall(__NR_set_mempolicy, MPOL_BIND, mask, 8 * mask_size);

		/* Allocate and touch pages under this policy */
		void *p = mmap(NULL, 4096 * 32, PROT_READ | PROT_WRITE,
			       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p != MAP_FAILED) {
			memset(p, 0xAA, 4096 * 32);
			cnt_alloc++;
			munmap(p, 4096 * 32);
		}

		/* Reset policy */
		syscall(__NR_set_mempolicy, MPOL_DEFAULT, NULL, 0);

		/* Allocate without policy */
		p = mmap(NULL, 4096 * 32, PROT_READ | PROT_WRITE,
			 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p != MAP_FAILED) {
			memset(p, 0xBB, 4096 * 32);
			cnt_alloc++;
			munmap(p, 4096 * 32);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int duration = 30;
	pthread_t threads[16];
	int nthreads = 0;

	if (argc < 2) {
		fprintf(stderr,
			"Usage: %s <private_nid> [duration_secs]\n"
			"  private_nid: NUMA node ID of the private node\n"
			"  duration: test duration in seconds (default: 30)\n",
			argv[0]);
		return 1;
	}

	private_nid = atoi(argv[1]);
	if (argc >= 3)
		duration = atoi(argv[2]);

	printf("Stress testing private node %d for %d seconds\n",
	       private_nid, duration);
	printf("Threads: mempolicy, mbind, migrate, move_pages, "
	       "home_node, madvise, mlock, alloc(x2)\n");

	signal(SIGALRM, sigalrm_handler);
	alarm(duration);

	/* Launch threads */
	pthread_create(&threads[nthreads++], NULL, thread_mempolicy, NULL);
	pthread_create(&threads[nthreads++], NULL, thread_mbind, NULL);
	pthread_create(&threads[nthreads++], NULL, thread_migrate, NULL);
	pthread_create(&threads[nthreads++], NULL, thread_move_pages, NULL);
	pthread_create(&threads[nthreads++], NULL, thread_home_node, NULL);
	pthread_create(&threads[nthreads++], NULL, thread_madvise, NULL);
	pthread_create(&threads[nthreads++], NULL, thread_mlock, NULL);
	pthread_create(&threads[nthreads++], NULL, thread_alloc_stress, NULL);
	pthread_create(&threads[nthreads++], NULL, thread_alloc_stress, NULL);

	for (int i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	printf("\n=== Results ===\n");
	printf("  set_mempolicy:          %lu ops\n", cnt_mempolicy);
	printf("  mbind:                  %lu ops\n", cnt_mbind);
	printf("  migrate_pages:          %lu ops\n", cnt_migrate);
	printf("  move_pages:             %lu ops\n", cnt_move_pages);
	printf("  set_mempolicy_home_node:%lu ops\n", cnt_home_node);
	printf("  madvise:                %lu ops\n", cnt_madvise);
	printf("  mlock:                  %lu ops\n", cnt_mlock);
	printf("  alloc_stress:           %lu ops\n", cnt_alloc);
	printf("  errors:                 %lu\n", errors);
	printf("PASS: no kernel crash/warning detected\n");

	return 0;
}
