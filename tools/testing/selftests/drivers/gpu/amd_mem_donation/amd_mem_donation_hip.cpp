// SPDX-License-Identifier: GPL-2.0
/*
 * HIP allocation and integrity worker for AMDGPU reversible UMA donation.
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
#include <getopt.h>
#include <limits>
#include <thread>
#include <vector>

struct allocation {
	std::uint64_t *pointer;
	std::size_t words;
	std::uint64_t seed;
};

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

__device__ static std::uint64_t pattern(std::uint64_t index,
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

static void launch_fill(const allocation &item)
{
	constexpr unsigned int threads = 256;
	const unsigned int blocks = std::min<std::size_t>(
		4096, (item.words + threads - 1) / threads);

	hipLaunchKernelGGL(fill_kernel, dim3(blocks), dim3(threads), 0, 0,
			   item.pointer, item.words, item.seed);
	hip_fatal(hipGetLastError(), "fill kernel launch");
}

static std::uint64_t verify_allocations(
	const std::vector<allocation> &allocations,
	unsigned long long *device_errors)
{
	constexpr unsigned int threads = 256;
	unsigned long long host_errors = 0;

	hip_fatal(hipMemset(device_errors, 0, sizeof(*device_errors)),
		  "clear error counter");
	for (const auto &item : allocations) {
		const unsigned int blocks = std::min<std::size_t>(
			4096, (item.words + threads - 1) / threads);

		hipLaunchKernelGGL(verify_kernel, dim3(blocks), dim3(threads),
				   0, 0, item.pointer, item.words, item.seed,
				   device_errors);
		hip_fatal(hipGetLastError(), "verify kernel launch");
	}
	hip_fatal(hipDeviceSynchronize(), "verify synchronize");
	hip_fatal(hipMemcpy(&host_errors, device_errors, sizeof(host_errors),
			   hipMemcpyDeviceToHost),
		  "copy error counter");
	return host_errors;
}

static void usage(const char *program)
{
	std::fprintf(stderr,
		     "Usage: %s --mib N --seconds N [--chunk-mib N] "
		     "[--device N] [--pci-bdf BDF] [--require-bytes] "
		     "[--wait-for-start]\n",
		     program);
}

int main(int argc, char **argv)
{
	static const option options[] = {
		{ "mib", required_argument, nullptr, 'm' },
		{ "seconds", required_argument, nullptr, 't' },
		{ "chunk-mib", required_argument, nullptr, 'c' },
		{ "device", required_argument, nullptr, 'd' },
		{ "pci-bdf", required_argument, nullptr, 'b' },
		{ "require-bytes", no_argument, nullptr, 'r' },
		{ "wait-for-start", no_argument, nullptr, 'w' },
		{ "help", no_argument, nullptr, 'h' },
		{ }
	};
	std::uint64_t requested_mib = 0;
	std::uint64_t duration = 0;
	std::uint64_t chunk_mib = 256;
	std::uint64_t allocated_bytes = 0;
	std::uint64_t verify_errors = 0;
	std::uint64_t rounds = 0;
	int device = 0;
	int allocation_error = hipSuccess;
	bool require_bytes = false;
	bool wait_for_start = false;
	const char *pci_bdf = nullptr;
	int selected_device;
	int device_count;
	int option;

	while ((option = getopt_long(argc, argv, "m:t:c:d:b:rwh",
				     options, nullptr)) != -1) {
		switch (option) {
		case 'm':
			requested_mib = parse_u64("MiB", optarg);
			break;
		case 't':
			duration = parse_u64("seconds", optarg);
			break;
		case 'c':
			chunk_mib = parse_u64("chunk MiB", optarg);
			break;
		case 'd':
			device = static_cast<int>(parse_u64("device", optarg));
			break;
		case 'b':
			pci_bdf = optarg;
			break;
		case 'r':
			require_bytes = true;
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

	if (!requested_mib || !duration || !chunk_mib) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (requested_mib > std::numeric_limits<std::size_t>::max() /
				    (1024 * 1024ULL) ||
	    chunk_mib > std::numeric_limits<std::size_t>::max() /
			 (1024 * 1024ULL)) {
		std::fprintf(stderr, "requested allocation is too large\n");
		return EXIT_FAILURE;
	}

	hip_fatal(hipGetDeviceCount(&device_count), "hipGetDeviceCount");
	if (pci_bdf)
		hip_fatal(hipDeviceGetByPCIBusId(&device, pci_bdf),
			  "hipDeviceGetByPCIBusId");
	else if (device < 0 || device >= device_count) {
		std::fprintf(stderr, "device %d is unavailable (count=%d)\n",
			     device, device_count);
		return EXIT_FAILURE;
	}
	hip_fatal(hipSetDevice(device), "hipSetDevice");
	hip_fatal(hipGetDevice(&selected_device), "hipGetDevice");

	char device_name[256] = {};
	hipDeviceProp_t properties = {};
	hip_fatal(hipGetDeviceProperties(&properties, selected_device),
		  "hipGetDeviceProperties");
	std::snprintf(device_name, sizeof(device_name), "%s", properties.name);

	std::size_t free_before;
	std::size_t total_before;
	hip_fatal(hipMemGetInfo(&free_before, &total_before), "hipMemGetInfo");

	std::signal(SIGINT, stop_handler);
	std::signal(SIGTERM, stop_handler);
	std::signal(SIGUSR1, start_handler);

	std::vector<allocation> allocations;
	const std::uint64_t requested_bytes = requested_mib * 1024 * 1024ULL;
	const std::uint64_t maximum_chunk = chunk_mib * 1024 * 1024ULL;

	while (allocated_bytes < requested_bytes) {
		std::uint64_t this_size =
			std::min(maximum_chunk, requested_bytes - allocated_bytes);
		void *pointer = nullptr;
		hipError_t error = hipMalloc(&pointer, this_size);

		if (error != hipSuccess) {
			allocation_error = error;
			(void)hipGetLastError();
			break;
		}

		allocation item = {
			.pointer = static_cast<std::uint64_t *>(pointer),
			.words = static_cast<std::size_t>(
				this_size / sizeof(std::uint64_t)),
			.seed = 0x616d646d656dULL ^ allocations.size(),
		};
		allocations.push_back(item);
		allocated_bytes += this_size;
		launch_fill(item);
	}
	hip_fatal(hipDeviceSynchronize(), "fill synchronize");

	std::size_t free_after;
	std::size_t total_after;
	hip_fatal(hipMemGetInfo(&free_after, &total_after), "hipMemGetInfo");
	if (allocations.empty()) {
		std::fprintf(stderr, "HIP allocated no pressure buffers\n");
		return EXIT_FAILURE;
	}
	std::printf("GPU_READY device=%d name=%s requested_bytes=%" PRIu64
		    " allocated_bytes=%" PRIu64 " allocation_error=%d "
		    "free_before=%zu total_before=%zu free_after=%zu "
		    "total_after=%zu\n",
		    selected_device, device_name, requested_bytes, allocated_bytes,
		    allocation_error, free_before, total_before, free_after,
		    total_after);
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
		verify_errors +=
			verify_allocations(allocations, device_errors);
		rounds++;
		if (verify_errors || stop_requested.load() ||
		    std::chrono::steady_clock::now() >= deadline)
			break;
	} while (true);

	std::printf("RESULT requested_bytes=%" PRIu64
		    " allocated_bytes=%" PRIu64 " rounds=%" PRIu64
		    " allocation_error=%d verify_errors=%" PRIu64 "\n",
		    requested_bytes, allocated_bytes, rounds, allocation_error,
		    verify_errors);
	std::fflush(stdout);

	hip_fatal(hipFree(device_errors), "free error counter");
	for (const auto &item : allocations)
		hip_fatal(hipFree(item.pointer), "free pressure allocation");

	if (verify_errors)
		return EXIT_FAILURE;
	if (require_bytes && allocated_bytes != requested_bytes)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
