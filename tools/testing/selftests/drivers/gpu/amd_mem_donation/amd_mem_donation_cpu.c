// SPDX-License-Identifier: GPL-2.0
/*
 * Anonymous-memory integrity worker for AMDGPU reversible UMA donation tests.
 *
 * The worker fills an allocation with a deterministic pattern, repeatedly
 * verifies it, and (when permitted to read PFNs) reports how many pages reside
 * inside the advertised donation range.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PAGEMAP_PRESENT		UINT64_C(0x8000000000000000)
#define PAGEMAP_PFN_MASK	UINT64_C(0x007fffffffffffff)
#define PAGEMAP_BATCH		4096

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t start_requested;

static void stop_handler(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void start_handler(int signo)
{
	(void)signo;
	start_requested = 1;
}

static double monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}

	return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static uint64_t pattern(uint64_t index, uint64_t seed)
{
	uint64_t value = index + 0x9e3779b97f4a7c15ULL;

	value ^= seed;
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

static uint64_t parse_u64(const char *name, const char *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(value, &end, 0);
	if (errno || !*value || *end) {
		fprintf(stderr, "invalid %s: %s\n", name, value);
		exit(EXIT_FAILURE);
	}

	return parsed;
}

static uint64_t count_range_pages(void *mapping, size_t length,
				  uint64_t range_start, uint64_t range_size,
				  bool *pfn_visible)
{
	uint64_t entries[PAGEMAP_BATCH];
	const long page_size = sysconf(_SC_PAGESIZE);
	const uintptr_t first_page = (uintptr_t)mapping / page_size;
	const size_t nr_pages = length / page_size;
	const uint64_t range_end = range_start + range_size;
	uint64_t in_range = 0;
	bool any_pfn = false;
	size_t done = 0;
	int fd;

	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		*pfn_visible = false;
		return 0;
	}

	while (done < nr_pages) {
		size_t batch = nr_pages - done;
		off_t offset;
		ssize_t bytes;
		size_t i;

		if (batch > PAGEMAP_BATCH)
			batch = PAGEMAP_BATCH;
		offset = (off_t)(first_page + done) * sizeof(entries[0]);
		bytes = pread(fd, entries, batch * sizeof(entries[0]), offset);
		if (bytes != (ssize_t)(batch * sizeof(entries[0]))) {
			fprintf(stderr, "short pagemap read at page %zu: %s\n",
				done, bytes < 0 ? strerror(errno) : "short read");
			close(fd);
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < batch; i++) {
			uint64_t entry = entries[i];
			uint64_t pfn;
			uint64_t physical;

			if (!(entry & PAGEMAP_PRESENT))
				continue;
			pfn = entry & PAGEMAP_PFN_MASK;
			if (!pfn)
				continue;
			any_pfn = true;
			physical = pfn * page_size;
			if (physical >= range_start && physical < range_end)
				in_range++;
		}
		done += batch;
	}

	close(fd);
	*pfn_visible = any_pfn;
	return in_range;
}

static uint64_t verify_mapping(const uint64_t *words, size_t nr_words,
			       uint64_t seed)
{
	uint64_t errors = 0;
	size_t i;

	for (i = 0; i < nr_words; i++) {
		uint64_t expected = pattern(i, seed);

		if (words[i] != expected) {
			if (errors < 8)
				fprintf(stderr,
					"mismatch word=%zu expected=%#llx actual=%#llx\n",
					i, (unsigned long long)expected,
					(unsigned long long)words[i]);
			errors++;
		}
	}

	return errors;
}

static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s --mib N --seconds N --range-start ADDR",
		program);
	fputs(" --range-size BYTES [--seed N] [--wait-for-start]\n", stderr);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "mib", required_argument, NULL, 'm' },
		{ "seconds", required_argument, NULL, 't' },
		{ "range-start", required_argument, NULL, 's' },
		{ "range-size", required_argument, NULL, 'z' },
		{ "seed", required_argument, NULL, 'r' },
		{ "wait-for-start", no_argument, NULL, 'w' },
		{ "help", no_argument, NULL, 'h' },
		{ }
	};
	uint64_t range_start = 0;
	uint64_t range_size = 0;
	uint64_t seed = 0x616d646d656dULL;
	uint64_t requested_mib = 0;
	uint64_t duration = 0;
	uint64_t range_pages = 0;
	uint64_t verify_errors = 0;
	const long page_size = sysconf(_SC_PAGESIZE);
	size_t length;
	size_t nr_words;
	uint64_t *words;
	double deadline;
	unsigned int rounds = 0;
	bool pfn_visible = false;
	bool wait_for_start = false;
	int option;
	size_t i;

	while ((option = getopt_long(argc, argv, "m:t:s:z:r:wh",
				     options, NULL)) != -1) {
		switch (option) {
		case 'm':
			requested_mib = parse_u64("MiB", optarg);
			break;
		case 't':
			duration = parse_u64("seconds", optarg);
			break;
		case 's':
			range_start = parse_u64("range start", optarg);
			break;
		case 'z':
			range_size = parse_u64("range size", optarg);
			break;
		case 'r':
			seed = parse_u64("seed", optarg);
			break;
		case 'w':
			wait_for_start = true;
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!requested_mib || !duration || !range_size || page_size <= 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (requested_mib > SIZE_MAX / (1024 * 1024ULL)) {
		fprintf(stderr, "allocation is too large\n");
		return EXIT_FAILURE;
	}

	length = requested_mib * 1024 * 1024ULL;
	length -= length % page_size;
	nr_words = length / sizeof(*words);
	words = mmap(NULL, length, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (words == MAP_FAILED) {
		fprintf(stderr, "mmap %zu bytes failed: %s\n",
			length, strerror(errno));
		return EXIT_FAILURE;
	}

	if (madvise(words, length, MADV_NOHUGEPAGE))
		fprintf(stderr, "MADV_NOHUGEPAGE failed: %s\n", strerror(errno));

	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	signal(SIGUSR1, start_handler);

	for (i = 0; i < nr_words; i++)
		words[i] = pattern(i, seed);

	range_pages = count_range_pages(words, length, range_start, range_size,
					&pfn_visible);
	printf("CPU_READY allocated_bytes=%zu", length);
	printf(" range_pages=%llu pfn_visible=%u\n",
	       (unsigned long long)range_pages, pfn_visible);
	fflush(stdout);
	while (wait_for_start && !start_requested && !stop_requested)
		usleep(10000);

	deadline = monotonic_seconds() + duration;
	do {
		uint64_t errors = verify_mapping(words, nr_words, seed);

		verify_errors += errors;
		rounds++;
		if (errors)
			break;
		if (monotonic_seconds() >= deadline || stop_requested)
			break;
	} while (true);

	/* Recount after pressure/migration to observe final residency. */
	range_pages = count_range_pages(words, length, range_start, range_size,
					&pfn_visible);
	printf("RESULT requested_bytes=%zu allocated_bytes=%zu rounds=%u",
	       length, length, rounds);
	printf(" range_pages=%llu pfn_visible=%u verify_errors=%llu\n",
	       (unsigned long long)range_pages, pfn_visible,
	       (unsigned long long)verify_errors);
	fflush(stdout);

	munmap(words, length);
	return verify_errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
