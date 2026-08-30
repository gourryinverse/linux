// SPDX-License-Identifier: GPL-2.0
/*
 * Raw libdrm allocation plus HIP integrity worker for AMDGPU UMA donation.
 *
 * libdrm creates BOs with an explicit requested placement.  Each BO is
 * exported as dma-buf, imported as HIP external memory, and filled/verified by
 * GPU kernels.  The controller can correlate this process with
 * debugfs/amdgpu_gem_info to observe the BO's current placement.
 */

#include <hip/hip_runtime.h>

#include <amdgpu.h>
#include <amdgpu_drm.h>

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
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define PAGEMAP_PRESENT		(1ULL << 63)
#define PAGEMAP_PFN_MASK	((1ULL << 55) - 1)
#define PAGEMAP_BATCH		4096

struct allocation {
	amdgpu_bo_handle bo;
	hipExternalMemory_t external;
	std::uint64_t *pointer;
	std::size_t words;
	std::uint64_t size;
	std::uint64_t seed;
	void *cpu_pointer;
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

static void drm_fatal(int error, const char *operation)
{
	if (!error)
		return;
	std::fprintf(stderr, "%s failed: %s (%d)\n", operation,
		     std::strerror(error < 0 ? -error : error), error);
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

static void query_heap(amdgpu_device_handle device, std::uint32_t domain,
		       amdgpu_heap_info *info, const char *operation)
{
	drm_fatal(amdgpu_query_heap_info(device, domain, 0, info), operation);
}

static std::uint64_t fault_and_sample_cpu_pages(
	const std::vector<allocation> &allocations, std::uint64_t *errors)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	std::uint64_t checksum = 0;

	if (page_size <= 0)
		return 0;
	for (const auto &item : allocations) {
		if (!item.cpu_pointer)
			continue;
		volatile std::uint64_t *words =
			static_cast<volatile std::uint64_t *>(item.cpu_pointer);
		const std::size_t stride = page_size / sizeof(std::uint64_t);

		for (std::size_t index = 0; index < item.words; index += stride) {
			std::uint64_t value = words[index];

			checksum ^= value;
			if (value != pattern(index, item.seed))
				(*errors)++;
		}
	}
	return checksum;
}

static std::uint64_t count_range_pages(
	const std::vector<allocation> &allocations, std::uint64_t range_start,
	std::uint64_t range_size, bool *pfn_visible)
{
	std::uint64_t entries[PAGEMAP_BATCH];
	const long page_size = sysconf(_SC_PAGESIZE);
	const std::uint64_t range_end = range_start + range_size;
	std::uint64_t in_range = 0;
	bool any_pfn = false;
	int fd;

	*pfn_visible = false;
	if (page_size <= 0 || !range_size)
		return 0;
	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;

	for (const auto &item : allocations) {
		if (!item.cpu_pointer)
			continue;
		const std::uintptr_t first_page =
			reinterpret_cast<std::uintptr_t>(item.cpu_pointer) / page_size;
		const std::size_t nr_pages = item.size / page_size;
		std::size_t done = 0;

		while (done < nr_pages) {
			std::size_t batch = std::min<std::size_t>(
				PAGEMAP_BATCH, nr_pages - done);
			off_t offset = static_cast<off_t>(first_page + done) *
				       sizeof(entries[0]);
			ssize_t bytes = pread(fd, entries,
					      batch * sizeof(entries[0]), offset);

			if (bytes != static_cast<ssize_t>(
					     batch * sizeof(entries[0]))) {
				std::fprintf(stderr,
					     "short pagemap read at page %zu: %s\n",
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
	}

	close(fd);
	*pfn_visible = any_pfn;
	return in_range;
}

static void usage(const char *program)
{
	std::fprintf(stderr,
		     "Usage: %s --mib N --seconds N [--chunk-mib N] "
		     "[--domain vram|gtt|both] [--render-node PATH] "
		     "[--device N] [--pci-bdf BDF] [--range-start ADDR] "
		     "[--range-size BYTES] [--contiguous] [--require-bytes] "
		     "[--wait-for-start]\n",
		     program);
}

int main(int argc, char **argv)
{
	static const option options[] = {
		{ "mib", required_argument, nullptr, 'm' },
		{ "seconds", required_argument, nullptr, 't' },
		{ "chunk-mib", required_argument, nullptr, 'c' },
		{ "domain", required_argument, nullptr, 'p' },
		{ "render-node", required_argument, nullptr, 'n' },
		{ "device", required_argument, nullptr, 'd' },
		{ "pci-bdf", required_argument, nullptr, 'b' },
		{ "range-start", required_argument, nullptr, 's' },
		{ "range-size", required_argument, nullptr, 'z' },
		{ "contiguous", no_argument, nullptr, 'C' },
		{ "require-bytes", no_argument, nullptr, 'r' },
		{ "wait-for-start", no_argument, nullptr, 'w' },
		{ "help", no_argument, nullptr, 'h' },
		{ }
	};
	std::uint64_t requested_mib = 0;
	std::uint64_t duration = 0;
	std::uint64_t chunk_mib = 256;
	std::uint64_t allocated_bytes = 0;
	std::uint64_t raw_allocated_bytes = 0;
	std::uint64_t range_start = 0;
	std::uint64_t range_size = 0;
	std::uint64_t cpu_sample_errors = 0;
	std::uint64_t cpu_checksum = 0;
	std::uint64_t range_pages = 0;
	bool pfn_visible = false;
	std::uint64_t verify_errors = 0;
	std::uint64_t rounds = 0;
	std::uint32_t domain = AMDGPU_GEM_DOMAIN_VRAM;
	const char *domain_name = "vram";
	const char *render_node = "/dev/dri/renderD128";
	bool contiguous = false;
	bool require_bytes = false;
	bool wait_for_start = false;
	int allocation_error = 0;
	int raw_allocation_error = 0;
	const char *allocation_stage = "none";
	int hip_device = 0;
	const char *pci_bdf = nullptr;
	int option;

	while ((option = getopt_long(argc, argv, "m:t:c:p:n:d:b:s:z:Crwh",
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
		case 'p':
			if (!std::strcmp(optarg, "vram")) {
				domain = AMDGPU_GEM_DOMAIN_VRAM;
				domain_name = "vram";
			} else if (!std::strcmp(optarg, "gtt")) {
				domain = AMDGPU_GEM_DOMAIN_GTT;
				domain_name = "gtt";
			} else if (!std::strcmp(optarg, "both")) {
				domain = AMDGPU_GEM_DOMAIN_VRAM |
					 AMDGPU_GEM_DOMAIN_GTT;
				domain_name = "both";
			} else {
				std::fprintf(stderr, "invalid domain: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'n':
			render_node = optarg;
			break;
		case 'd':
			hip_device = static_cast<int>(parse_u64("device", optarg));
			break;
		case 'b':
			pci_bdf = optarg;
			break;
		case 's':
			range_start = parse_u64("range start", optarg);
			break;
		case 'z':
			range_size = parse_u64("range size", optarg);
			break;
		case 'C':
			contiguous = true;
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

	if (pci_bdf)
		hip_fatal(hipDeviceGetByPCIBusId(&hip_device, pci_bdf),
			  "hipDeviceGetByPCIBusId");
	hip_fatal(hipSetDevice(hip_device), "hipSetDevice");
	int drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		std::fprintf(stderr, "open %s failed: %s\n", render_node,
			     std::strerror(errno));
		return EXIT_FAILURE;
	}

	std::uint32_t drm_major;
	std::uint32_t drm_minor;
	amdgpu_device_handle drm_device = nullptr;
	drm_fatal(amdgpu_device_initialize(drm_fd, &drm_major, &drm_minor,
					   &drm_device),
		  "amdgpu_device_initialize");

	amdgpu_heap_info vram_before = {};
	amdgpu_heap_info gtt_before = {};
	query_heap(drm_device, AMDGPU_GEM_DOMAIN_VRAM, &vram_before,
		   "query VRAM before");
	query_heap(drm_device, AMDGPU_GEM_DOMAIN_GTT, &gtt_before,
		   "query GTT before");

	std::signal(SIGINT, stop_handler);
	std::signal(SIGTERM, stop_handler);
	std::signal(SIGUSR1, start_handler);

	std::vector<allocation> allocations;
	std::vector<amdgpu_bo_handle> diagnostic_bos;
	const std::uint64_t requested_bytes = requested_mib * 1024 * 1024ULL;
	const std::uint64_t maximum_chunk = chunk_mib * 1024 * 1024ULL;

	while (allocated_bytes < requested_bytes) {
		std::uint64_t this_size =
			std::min(maximum_chunk, requested_bytes - allocated_bytes);
		amdgpu_bo_alloc_request request = {};
		request.alloc_size = this_size;
		request.phys_alignment = 4096;
		request.preferred_heap = domain;
		if (domain == AMDGPU_GEM_DOMAIN_GTT)
			request.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
		else
			request.flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
		if (contiguous)
			request.flags |= AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS;

		amdgpu_bo_handle bo = nullptr;
		int error = amdgpu_bo_alloc(drm_device, &request, &bo);
		if (error) {
			allocation_error = error;
			allocation_stage = "drm_alloc";
			break;
		}

		std::uint32_t dma_fd;
		error = amdgpu_bo_export(bo, amdgpu_bo_handle_type_dma_buf_fd,
					 &dma_fd);
		if (error)
			drm_fatal(error, "export BO as dma-buf");

		hipExternalMemoryHandleDesc external_desc = {};
		external_desc.type = hipExternalMemoryHandleTypeOpaqueFd;
		external_desc.handle.fd = static_cast<int>(dma_fd);
		external_desc.size = this_size;

		hipExternalMemory_t external = nullptr;
		hipError_t hip_error =
			hipImportExternalMemory(&external, &external_desc);
		if (hip_error != hipSuccess) {
			allocation_error = hip_error;
			allocation_stage = "hip_import";
			(void)hipGetLastError();
			close(static_cast<int>(dma_fd));
			diagnostic_bos.push_back(bo);
			raw_allocated_bytes = allocated_bytes + this_size;
			break;
		}

		hipExternalMemoryBufferDesc buffer_desc = {};
		buffer_desc.offset = 0;
		buffer_desc.size = this_size;
		void *pointer = nullptr;
		hip_error = hipExternalMemoryGetMappedBuffer(&pointer, external,
						     &buffer_desc);
		if (hip_error != hipSuccess) {
			allocation_error = hip_error;
			allocation_stage = "hip_map";
			(void)hipGetLastError();
			hip_fatal(hipDestroyExternalMemory(external),
				  "destroy unmapped external memory");
			diagnostic_bos.push_back(bo);
			raw_allocated_bytes = allocated_bytes + this_size;
			break;
		}

		void *cpu_pointer = nullptr;
		if (domain == AMDGPU_GEM_DOMAIN_GTT)
			drm_fatal(amdgpu_bo_cpu_map(bo, &cpu_pointer),
				  "CPU-map GTT BO");

		allocation item = {
			.bo = bo,
			.external = external,
			.pointer = static_cast<std::uint64_t *>(pointer),
			.words = static_cast<std::size_t>(
				this_size / sizeof(std::uint64_t)),
			.size = this_size,
			.seed = 0x64726d686970ULL ^ allocations.size(),
			.cpu_pointer = cpu_pointer,
		};
		allocations.push_back(item);
		allocated_bytes += this_size;
		raw_allocated_bytes = allocated_bytes;
		launch_fill(item);
	}
	hip_fatal(hipDeviceSynchronize(), "fill synchronize");
	cpu_checksum = fault_and_sample_cpu_pages(allocations,
						 &cpu_sample_errors);
	range_pages = count_range_pages(allocations, range_start, range_size,
					&pfn_visible);

	/*
	 * HIP may impose a smaller external-memory limit than GEM creation.
	 * Keep allocating raw BOs after that point so debugfs can show whether
	 * the kernel falls back to another domain.  These BOs are deliberately
	 * reported separately and are not credited as GPU-verified bytes.
	 */
	while (raw_allocated_bytes < requested_bytes) {
		std::uint64_t this_size = std::min(
			maximum_chunk, requested_bytes - raw_allocated_bytes);
		amdgpu_bo_alloc_request request = {};
		request.alloc_size = this_size;
		request.phys_alignment = 4096;
		request.preferred_heap = domain;
		if (domain == AMDGPU_GEM_DOMAIN_GTT)
			request.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
		else
			request.flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
		if (contiguous)
			request.flags |= AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS;

		amdgpu_bo_handle bo = nullptr;
		int error = amdgpu_bo_alloc(drm_device, &request, &bo);
		if (error) {
			raw_allocation_error = error;
			break;
		}
		diagnostic_bos.push_back(bo);
		raw_allocated_bytes += this_size;
	}

	amdgpu_heap_info vram_after = {};
	amdgpu_heap_info gtt_after = {};
	query_heap(drm_device, AMDGPU_GEM_DOMAIN_VRAM, &vram_after,
		   "query VRAM after");
	query_heap(drm_device, AMDGPU_GEM_DOMAIN_GTT, &gtt_after,
		   "query GTT after");
	if (allocations.empty()) {
		std::fprintf(stderr, "libdrm allocated no pressure buffers\n");
		return EXIT_FAILURE;
	}

	std::printf("DRM_GPU_READY pid=%ld domain=%s requested_bytes=%" PRIu64
		    " allocated_bytes=%" PRIu64 " raw_allocated_bytes=%" PRIu64
		    " allocation_error=%d raw_allocation_error=%d "
		    "allocation_stage=%s "
		    "range_pages=%" PRIu64 " pfn_visible=%u "
		    "cpu_sample_errors=%" PRIu64 " cpu_checksum=%" PRIu64 " "
		    "contiguous=%u drm_major=%u drm_minor=%u "
		    "vram_usage_before=%" PRIu64 " vram_usage_after=%" PRIu64
		    " gtt_usage_before=%" PRIu64 " gtt_usage_after=%" PRIu64
		    "\n",
		    static_cast<long>(getpid()), domain_name, requested_bytes,
		    allocated_bytes, raw_allocated_bytes, allocation_error,
		    raw_allocation_error, allocation_stage, range_pages, pfn_visible,
		    cpu_sample_errors, cpu_checksum, contiguous, drm_major, drm_minor,
		    vram_before.heap_usage, vram_after.heap_usage,
		    gtt_before.heap_usage, gtt_after.heap_usage);
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
		verify_errors += verify_allocations(allocations, device_errors);
		rounds++;
		if (verify_errors || stop_requested.load() ||
		    std::chrono::steady_clock::now() >= deadline)
			break;
	} while (true);

	query_heap(drm_device, AMDGPU_GEM_DOMAIN_VRAM, &vram_after,
		   "query final VRAM");
	query_heap(drm_device, AMDGPU_GEM_DOMAIN_GTT, &gtt_after,
		   "query final GTT");
	range_pages = count_range_pages(allocations, range_start, range_size,
					&pfn_visible);
	std::printf("RESULT requested_bytes=%" PRIu64
		    " allocated_bytes=%" PRIu64 " raw_allocated_bytes=%" PRIu64
		    " rounds=%" PRIu64 " allocation_error=%d "
		    "raw_allocation_error=%d allocation_stage=%s "
		    "verify_errors=%" PRIu64 " range_pages=%" PRIu64
		    " pfn_visible=%u cpu_sample_errors=%" PRIu64
		    " vram_usage_final=%" PRIu64 " gtt_usage_final=%" PRIu64
		    "\n",
		    requested_bytes, allocated_bytes, raw_allocated_bytes, rounds,
		    allocation_error, raw_allocation_error, allocation_stage,
		    verify_errors, range_pages, pfn_visible, cpu_sample_errors,
		    vram_after.heap_usage, gtt_after.heap_usage);
	std::fflush(stdout);

	hip_fatal(hipFree(device_errors), "free error counter");
	for (auto bo = diagnostic_bos.rbegin(); bo != diagnostic_bos.rend(); ++bo)
		drm_fatal(amdgpu_bo_free(*bo), "free diagnostic BO");
	for (auto item = allocations.rbegin(); item != allocations.rend(); ++item) {
		hip_fatal(hipDestroyExternalMemory(item->external),
			  "destroy external memory");
		if (item->cpu_pointer)
			drm_fatal(amdgpu_bo_cpu_unmap(item->bo),
				  "CPU-unmap GTT BO");
		drm_fatal(amdgpu_bo_free(item->bo), "free BO");
	}
	drm_fatal(amdgpu_device_deinitialize(drm_device),
		  "amdgpu_device_deinitialize");
	close(drm_fd);

	if (verify_errors || cpu_sample_errors)
		return EXIT_FAILURE;
	if (require_bytes && allocated_bytes != requested_bytes)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
