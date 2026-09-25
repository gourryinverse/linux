// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cram_demote.h"

#define NR_PAGES 4096
#define PATTERN 0xab

static volatile sig_atomic_t stop;

static void stop_handler(int sig)
{
	stop = 1;
}

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

int main(int argc, char **argv)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t len = NR_PAGES * page_size;
	unsigned char *ptr;
	int nid;

	if (argc != 2)
		return 2;
	nid = atoi(argv[1]);
	ptr = mmap(NULL, len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ptr == MAP_FAILED)
		return 2;
	memset(ptr, PATTERN, len);
	if (cram_pageout(ptr, len) || pages_on_node(ptr, nid) < NR_PAGES / 2)
		return 3;

	signal(SIGTERM, stop_handler);
	printf("ready %p\n", ptr);
	fflush(stdout);
	while (!stop)
		pause();
	return 0;
}
