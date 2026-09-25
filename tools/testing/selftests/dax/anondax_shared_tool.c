// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define NR_PAGES 64
#define PATTERN 0xab
#define CHILD_PATTERN 0xcd

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

int main(int argc, char **argv)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t len = NR_PAGES * page_size;
	unsigned char *first, *alias, *other, *private;
	int fd, other_fd, status;
	int private_rejected, shared, isolated, fork_shared;
	long resident;
	pid_t pid;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <device> <nid>\n", argv[0]);
		return 2;
	}

	fd = open(argv[1], O_RDWR);
	other_fd = open(argv[1], O_RDWR);
	if (fd < 0 || other_fd < 0) {
		perror("open");
		return 2;
	}

	errno = 0;
	private = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	private_rejected = private == MAP_FAILED && errno == EINVAL;
	if (private != MAP_FAILED)
		munmap(private, len);

	first = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	alias = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	other = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, other_fd, 0);
	if (first == MAP_FAILED || alias == MAP_FAILED || other == MAP_FAILED) {
		perror("mmap");
		return 2;
	}

	memset(first, PATTERN, len);
	shared = alias[0] == PATTERN && alias[len - 1] == PATTERN;
	isolated = other[0] == 0;
	other[0] = CHILD_PATTERN;
	isolated &= first[0] == PATTERN;
	resident = pages_on_node(first, atoi(argv[2]));

	pid = fork();
	if (!pid) {
		alias[0] = CHILD_PATTERN;
		_exit(0);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return 2;
	fork_shared = WIFEXITED(status) && !WEXITSTATUS(status) &&
		first[0] == CHILD_PATTERN;

	printf("private_rejected=%d shared=%d isolated=%d fork_shared=%d on_node=%ld\n",
	       private_rejected, shared, isolated, fork_shared, resident);

	munmap(other, len);
	munmap(alias, len);
	munmap(first, len);
	close(other_fd);
	close(fd);

	return private_rejected && shared && isolated && fork_shared &&
		resident == NR_PAGES ? 0 : 1;
}
