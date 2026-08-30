<!-- SPDX-License-Identifier: GPL-2.0 -->
# AMDGPU reversible UMA donation hardware tests

These tests exercise the experimental `uma/donated_memory_bytes` interface on
real AMDGPU UMA hardware. They are intentionally separate from the ordinary
kselftest run: changing memory ownership can expose firmware, GPU, memory
hotplug, and data-integrity failures severe enough to hang or reboot a machine.

The controller must run as root. It refuses kernels whose release does not
contain `.amdmem` unless `--allow-non-test-kernel` is supplied. Every run must
start with zero donated and quarantined bytes. A refusal caused by pre-existing
ownership leaves that state untouched. Once the controller has confirmed zero
initial ownership, success, failure, and SIGINT paths stop every worker before
making bounded attempts to return all memory to the GPU.
The controller protects itself from the OOM killer so it can perform that
cleanup, but its launcher restores the default OOM score before executing each
pressure helper (or preserves a less-protected inherited score); pressure
workers remain eligible OOM victims.

Build the CPU and HIP integrity workers with:

```sh
make
```

The HIP architecture defaults to `gfx1150`; override `HIP_ARCH` for another
GPU. Fedora's ROCm packages require an explicit `-lamdhip64`, which can be
overridden through `HIPLDLIBS`.

Run a minimal transition test first:

```sh
sudo ./amd_mem_donation.py --scenario smoke --cycles 3
```

Then increase coverage in stages:

```sh
sudo ./amd_mem_donation.py --scenario staircase \
  --targets one,1G,4G,8G,half,24G,28G,24G,half,8G,one,zero

sudo ./amd_mem_donation.py --scenario donate-then-pressure \
  --targets 28G --cpu-mib 4096 --gpu-mib 8192 --work-seconds 30

sudo ./amd_mem_donation.py --scenario donate-then-pressure \
  --targets 28G --cpu-mib 8192 --work-seconds 30 \
  --return-under-pressure

sudo ./amd_mem_donation.py --scenario donate-then-pressure \
  --targets 28G --userptr-mib 8192 --work-seconds 30 \
  --return-under-pressure

sudo ./amd_mem_donation.py --scenario donate-then-pressure \
  --targets 28G --drm-mib 8192 --drm-domain gtt \
  --require-drm-bytes --work-seconds 30 --return-under-pressure

sudo ./amd_mem_donation.py --scenario pressure-then-donate \
  --targets 28G --cpu-mib 4096 --gpu-mib 8192 --work-seconds 30

sudo ./amd_mem_donation.py --scenario random \
  --random-steps 100 --random-max 28G --hold-seconds 0.1

sudo ./amd_mem_donation.py --scenario invalid

sudo ./amd_mem_donation.py --scenario concurrent --cycles 20 \
  --targets zero,one,1G,4G,8G

sudo ./amd_mem_donation.py --scenario state-protection --cycles 20
```

`donate-then-pressure` asks HIP for more GPU memory than remains after a 28 GiB
donation on a 32 GiB UMA configuration. Allocation shortfall is an expected
policy outcome unless `--require-gpu-bytes` is specified; corruption, a worker
crash, quarantine, and kernel fault signatures always fail the test.
`--return-under-pressure` requests zero donation while patterned worker memory
is still live, then keeps verifying it. The return may succeed through page
migration or fail with a validated partial prefix when insufficient ordinary
System RAM remains; after stopping the workers, cleanup must still reach zero.

The raw DRM worker allocates exact-size BO chunks in `vram`, `gtt`, or `both`
domains and imports each BO into HIP for GPU fill/verify cycles. Use
`--require-drm-bytes` when allocation of the full requested byte count is part
of the assertion. On this APU, a `both` request may place an initial prefix in
VRAM and spill the rest to GTT. DRM special mappings do not expose useful PFNs
through `/proc/pagemap`, so use the userptr worker when the test must prove that
the workload's initial physical pages came from donated System RAM.

The userptr worker allocates anonymous CPU memory, records every initially
resident PFN through root-only pagemap, registers that memory with HIP, and
has the GPU replace the CPU's initial pattern before continuously verifying it.
When the worker starts after a nonzero donation, the controller requires its
root-visible pagemap sample to contain donated PFNs. `--userptr-mib` is
therefore the most direct test of hot-removing donated pages while KFD/HMM is
actively accessing them. Scale it in stages after a fresh boot; a kernel fault
can compromise the running kernel even when the controller later returns
donation accounting to zero.

All pressure helpers wait at a common start gate after allocating and reporting
readiness, so their timed verification intervals overlap. HIP helpers are
selected by the controller's PCI BDF, and the raw DRM helper also receives the
render node belonging to that PCI device. This avoids silently exercising a
different GPU on multi-GPU systems.

`state-protection` donates one block, verifies that a direct write of
`offline` to its `/sys/devices/system/memory/memoryX/state` file fails with
`EBUSY`, and then verifies that the AMDGPU aggregate control can still return
the block. This checks both sides of the memory-hotplug notifier authorization.

Each invocation creates `/var/tmp/amd-mem-donation-<timestamp>/` containing
JSON snapshots, kernel-log deltas, worker logs, an event stream, and a final
summary. A stable symlink points to the newest run. Watch it from another
terminal with:

```sh
sudo tail -F /var/tmp/amd-mem-donation-latest/events.jsonl
```

Ownership writes run in a separate process with a 300-second watchdog by
default. Override it with `--transition-timeout SECONDS`. If a writer remains
stuck inside the kernel after that deadline, the controller records the writer
PID and refuses to issue another ownership request rather than accumulating
blocked processes.

The controller always requires reported VRAM usage to be at least the donated
size. During otherwise idle transitions it also reports when non-donation VRAM
usage moves more than 64 MiB from its baseline. Such movement is expected when
a guard evicts a user BO, and returning memory does not automatically move that
BO back from GTT, so drift is diagnostic rather than a failure by default. Use
`--strict-vram-drift` on a quiescent/headless system to make it fatal, and
override the reporting threshold with `--vram-drift-mib N`.

Also monitor the kernel independently:

```sh
sudo dmesg -wH | rg --line-buffered -i \
  'amdgpu|hotplug|offline|memory|PAT|IOMMU|MCE|BUG|WARNING|fault'
```

Do not reset or unbind AMDGPU, suspend, hibernate, or reboot while
`donated_memory_bytes` is nonzero. Lifecycle coverage should be added only once
the transition and pressure tests are consistently clean and should explicitly
return ownership before any expected reboot.
