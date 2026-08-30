// SPDX-License-Identifier: GPL-2.0
/*
 * GPU integrity worker over registered anonymous System RAM.
 *
 * The process first faults ordinary anonymous pages, records how many PFNs are
 * in the donation interval, then registers the same mapping with HIP and uses
 * GPU kernels to fill and verify it while ownership transitions are attempted.
 */

#include <hip/hip_runtime.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <getopt.h>
#include <limits>
#include <sys/mman.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#define PAGEMAP_PRESENT		(1ULL << 63)
#define PAGEMAP_PFN_MASK	((1ULL << 55) - 1)
#define PAGEMAP_BATCH		4096

static std::atomic<bool> stop_requested;
static std::atomic<bool> start_requested;

static void stop_handler(int)
{
	stop_requested.store(true);
}

static void start_handler(int)
{
	start_requested.store(true);
}

static std::uint64_t parse_u64(const char *name, const char *value)
{
	char *end = nullptr;

	errno = 0;
	unsigned long long parsed = std::strtoull(value, &end, 0);
	if (errno || !*value || *end) {
		std::fprintf(stderr, "invalid %s: %s\n", name, value);
		std::exit(EXIT_FAILURE);
	}
	return parsed;
}

static void hip_fatal(hipError_t error, const char *operation)
{
	if (error == hipSuccess)
		return;
	std::fprintf(stderr, "%s failed: %s (%d)\n", operation,
		     hipGetErrorString(error), error);
	std::exit(EXIT_FAILURE);
}

__host__ __device__ static std::uint64_t pattern(std::uint64_t index,
						 std::uint64_t seed)
{
	std::uint64_t value = index + 0x9e3779b97f4a7c15ULL;

	value ^= seed;
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

__global__ static void fill_kernel(std::uint64_t *words, std::size_t nr_words,
				   std::uint64_t seed)
{
	std::size_t index = blockIdx.x * blockDim.x + threadIdx.x;
	std::size_t stride = blockDim.x * gridDim.x;

	for (; index < nr_words; index += stride)
		words[index] = pattern(index, seed);
}

__global__ static void verify_kernel(const std::uint64_t *words,
				     std::size_t nr_words,
				     std::uint64_t seed,
				     unsigned long long *errors)
{
	std::size_t index = blockIdx.x * blockDim.x + threadIdx.x;
	std::size_t stride = blockDim.x * gridDim.x;
	unsigned long long local_errors = 0;

	for (; index < nr_words; index += stride) {
		if (words[index] != pattern(index, seed))
			local_errors++;
	}
	if (local_errors)
		atomicAdd(errors, local_errors);
}

static std::uint64_t count_range_pages(void *mapping, std::size_t length,
				       std::uint64_t range_start,
				       std::uint64_t range_size,
				       bool *pfn_visible)
{
	std::uint64_t entries[PAGEMAP_BATCH];
	const long page_size = sysconf(_SC_PAGESIZE);
	const std::uintptr_t first_page =
		reinterpret_cast<std::uintptr_t>(mapping) / page_size;
	const std::size_t nr_pages = length / page_size;
	const std::uint64_t range_end = range_start + range_size;
	std::uint64_t in_range = 0;
	bool any_pfn = false;
	std::size_t done = 0;
	int fd;

	*pfn_visible = false;
	if (page_size <= 0 || !range_size)
		return 0;
	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;

	while (done < nr_pages) {
		std::size_t batch = std::min<std::size_t>(
			PAGEMAP_BATCH, nr_pages - done);
		off_t offset = static_cast<off_t>(first_page + done) *
			       sizeof(entries[0]);
		ssize_t bytes = pread(fd, entries,
				      batch * sizeof(entries[0]), offset);

		if (bytes != static_cast<ssize_t>(batch * sizeof(entries[0]))) {
			std::fprintf(stderr, "short pagemap read at page %zu: %s\n",
				     done, bytes < 0 ? std::strerror(errno) :
				     "short read");
			close(fd);
			std::exit(EXIT_FAILURE);
		}
		for (std::size_t index = 0; index < batch; index++) {
			std::uint64_t entry = entries[index];
			std::uint64_t pfn;

			if (!(entry & PAGEMAP_PRESENT))
				continue;
			pfn = entry & PAGEMAP_PFN_MASK;
			if (!pfn)
				continue;
			any_pfn = true;
			std::uint64_t physical = pfn * page_size;
			if (physical >= range_start && physical < range_end)
				in_range++;
		}
		done += batch;
	}

	close(fd);
	*pfn_visible = any_pfn;
	return in_range;
}

static std::uint64_t verify_cpu(const std::uint64_t *words,
				std::size_t nr_words, std::uint64_t seed)
{
	std::uint64_t errors = 0;

	for (std::size_t index = 0; index < nr_words; index++)
		if (words[index] != pattern(index, seed))
			errors++;
	return errors;
}

static void usage(const char *program)
{
	std::fprintf(stderr,
		     "Usage: %s --mib N --seconds N --range-start ADDR "
		     "--range-size BYTES [--device N] [--pci-bdf BDF] "
		     "[--wait-for-start]\n",
		     program);
}

int main(int argc, char **argv)
{
	static const option options[] = {
		{ "mib", required_argument, nullptr, 'm' },
		{ "seconds", required_argument, nullptr, 't' },
		{ "range-start", required_argument, nullptr, 's' },
		{ "range-size", required_argument, nullptr, 'z' },
		{ "device", required_argument, nullptr, 'd' },
		{ "pci-bdf", required_argument, nullptr, 'b' },
		{ "wait-for-start", no_argument, nullptr, 'w' },
		{ "help", no_argument, nullptr, 'h' },
		{ }
	};
	std::uint64_t requested_mib = 0;
	std::uint64_t duration = 0;
	std::uint64_t range_start = 0;
	std::uint64_t range_size = 0;
	std::uint64_t range_pages = 0;
	std::uint64_t verify_errors = 0;
	std::uint64_t cpu_verify_errors = 0;
	std::uint64_t rounds = 0;
	const std::uint64_t cpu_seed = 0x75736572707472ULL;
	const std::uint64_t gpu_seed = cpu_seed ^ 0x9e3779b97f4a7c15ULL;
	bool pfn_visible = false;
	bool wait_for_start = false;
	int device = 0;
	const char *pci_bdf = nullptr;
	int option;

	while ((option = getopt_long(argc, argv, "m:t:s:z:d:b:wh",
				     options, nullptr)) != -1) {
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
		case 'd':
			device = static_cast<int>(parse_u64("device", optarg));
			break;
		case 'b':
			pci_bdf = optarg;
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

	if (!requested_mib || !duration || !range_size ||
	    requested_mib > std::numeric_limits<std::size_t>::max() /
			    (1024 * 1024ULL)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	const std::size_t length = requested_mib * 1024 * 1024ULL;
	const std::size_t nr_words = length / sizeof(std::uint64_t);
	void *mapping = mmap(nullptr, length, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		std::fprintf(stderr, "mmap failed: %s\n", std::strerror(errno));
		return EXIT_FAILURE;
	}
	(void)madvise(mapping, length, MADV_NOHUGEPAGE);

	auto *host_words = static_cast<std::uint64_t *>(mapping);
	for (std::size_t index = 0; index < nr_words; index++)
		host_words[index] = pattern(index, cpu_seed);
	range_pages = count_range_pages(mapping, length, range_start, range_size,
					&pfn_visible);

	if (pci_bdf)
		hip_fatal(hipDeviceGetByPCIBusId(&device, pci_bdf),
			  "hipDeviceGetByPCIBusId");
	hip_fatal(hipSetDevice(device), "hipSetDevice");
	hip_fatal(hipHostRegister(mapping, length, hipHostRegisterMapped),
		  "hipHostRegister");
	void *device_mapping = nullptr;
	hip_fatal(hipHostGetDevicePointer(&device_mapping, mapping, 0),
		  "hipHostGetDevicePointer");
	auto *device_words = static_cast<std::uint64_t *>(device_mapping);

	constexpr unsigned int threads = 256;
	const unsigned int blocks = std::min<std::size_t>(
		4096, (nr_words + threads - 1) / threads);
	hipLaunchKernelGGL(fill_kernel, dim3(blocks), dim3(threads), 0, 0,
			   device_words, nr_words, gpu_seed);
	hip_fatal(hipGetLastError(), "fill kernel launch");
	hip_fatal(hipDeviceSynchronize(), "fill synchronize");
	cpu_verify_errors = verify_cpu(host_words, nr_words, gpu_seed);

	std::signal(SIGINT, stop_handler);
	std::signal(SIGTERM, stop_handler);
	std::signal(SIGUSR1, start_handler);
	std::printf("USERPTR_GPU_READY pid=%ld allocated_bytes=%zu "
		    "range_pages=%" PRIu64 " pfn_visible=%u "
		    "cpu_verify_errors=%" PRIu64 "\n",
		    static_cast<long>(getpid()), length, range_pages, pfn_visible,
		    cpu_verify_errors);
	std::fflush(stdout);
	while (wait_for_start && !start_requested.load() &&
	       !stop_requested.load())
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

	unsigned long long *device_errors = nullptr;
	hip_fatal(hipMalloc(&device_errors, sizeof(*device_errors)),
		  "allocate error counter");
	const auto deadline = std::chrono::steady_clock::now() +
			      std::chrono::seconds(duration);
	do {
		unsigned long long host_errors = 0;

		hip_fatal(hipMemset(device_errors, 0, sizeof(*device_errors)),
			  "clear error counter");
		hipLaunchKernelGGL(verify_kernel, dim3(blocks), dim3(threads),
				   0, 0, device_words, nr_words, gpu_seed,
				   device_errors);
		hip_fatal(hipGetLastError(), "verify kernel launch");
		hip_fatal(hipDeviceSynchronize(), "verify synchronize");
		hip_fatal(hipMemcpy(&host_errors, device_errors,
				   sizeof(host_errors), hipMemcpyDeviceToHost),
			  "copy error counter");
		verify_errors += host_errors;
		rounds++;
		if (verify_errors || stop_requested.load() ||
		    std::chrono::steady_clock::now() >= deadline)
			break;
	} while (true);

	range_pages = count_range_pages(mapping, length, range_start, range_size,
					&pfn_visible);
	std::printf("RESULT requested_bytes=%zu allocated_bytes=%zu rounds=%" PRIu64
		    " range_pages=%" PRIu64 " pfn_visible=%u "
		    "cpu_verify_errors=%" PRIu64 " verify_errors=%" PRIu64 "\n",
		    length, length, rounds, range_pages, pfn_visible,
		    cpu_verify_errors, verify_errors);
	std::fflush(stdout);

	hip_fatal(hipFree(device_errors), "free error counter");
	hip_fatal(hipHostUnregister(mapping), "hipHostUnregister");
	munmap(mapping, length);
	return verify_errors || cpu_verify_errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
