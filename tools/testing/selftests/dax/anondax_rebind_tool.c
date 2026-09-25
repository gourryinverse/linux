// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <linux/mempolicy.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NR_PAGES 64

static long count_node(void *addr, long page_size, int nid)
{
	void *pages[NR_PAGES];
	int status[NR_PAGES];
	long i, count = 0;

	for (i = 0; i < NR_PAGES; i++)
		pages[i] = addr + i * page_size;
	if (syscall(SYS_move_pages, 0, NR_PAGES, pages, NULL, status, 0UL))
		return -1;
	for (i = 0; i < NR_PAGES; i++)
		count += status[i] == nid;
	return count;
}

int main(int argc, char **argv)
{
	unsigned long mask;
	long page_size = sysconf(_SC_PAGESIZE);
	sigset_t waitset;
	void *ptr;
	int priv, common1, common2, mode, sig;

	if (argc != 5)
		return 2;
	mode = atoi(argv[1]);
	priv = atoi(argv[2]);
	common1 = atoi(argv[3]);
	common2 = atoi(argv[4]);
	if (priv >= (int)(8 * sizeof(mask)) || common1 >= (int)(8 * sizeof(mask)) ||
	    common2 >= (int)(8 * sizeof(mask)))
		return 3;

	sigemptyset(&waitset);
	sigaddset(&waitset, SIGUSR1);
	sigprocmask(SIG_BLOCK, &waitset, NULL);

	if (mode == 1) {
		mask = (1UL << common1) | (1UL << priv);
		if (syscall(SYS_set_mempolicy, MPOL_INTERLEAVE, &mask,
			    8 * sizeof(mask)))
			return 2;
		ptr = NULL;
	} else if (mode == 2) {
		mask = 1UL << common1;
		if (syscall(SYS_set_mempolicy, MPOL_BIND, &mask,
			    8 * sizeof(mask)))
			return 2;
		ptr = mmap(NULL, NR_PAGES * page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (ptr == MAP_FAILED)
			return 2;
		memset(ptr, 0xab, NR_PAGES * page_size);
	} else {
		return 2;
	}

	printf("ready\n");
	fflush(stdout);
	sigwait(&waitset, &sig);

	if (mode == 1) {
		ptr = mmap(NULL, NR_PAGES * page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (ptr == MAP_FAILED)
			return 2;
		memset(ptr, 0xcd, NR_PAGES * page_size);
	}

	printf("private=%ld old_common=%ld new_common=%ld\n",
	       count_node(ptr, page_size, priv),
	       count_node(ptr, page_size, common1),
	       count_node(ptr, page_size, common2));
	munmap(ptr, NR_PAGES * page_size);
	return 0;
}
