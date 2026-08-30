#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Fail-safe hardware harness for AMDGPU reversible UMA memory donation."""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import errno
import json
import os
import random
import re
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any


MIB = 1024 * 1024
GIB = 1024 * MIB
SYSFS_PCI = Path("/sys/bus/pci/devices")
SYSFS_MEMORY = Path("/sys/devices/system/memory")

KERNEL_FAILURE_PATTERNS = [
    re.compile(pattern, re.IGNORECASE)
    for pattern in (
        r"\bBUG:",
        r"\bWARNING:",
        r"\bOops:",
        r"\bkernel panic\b",
        r"\bMCE\b",
        r"Hardware Error",
        r"Bad page state",
        r"page allocation failure",
        r"x86/PAT:.*(?:failed|conflict|warning)",
        r"IOMMU.*(?:fault|event logged)",
        r"amdgpu.*(?:GPU fault|GPU reset|ring .*stalled|timeout|failed)",
        r"memory failure",
        r"hung task",
        r"soft lockup",
        r"hard lockup",
        r"rcu.*stall",
        r"UBSAN:",
        r"general protection fault",
    )
]


class TestFailure(RuntimeError):
    pass


@dataclass
class Worker:
    name: str
    process: subprocess.Popen[str]
    log_path: Path
    log_file: IO[str]
    ready_marker: str
    ready_details: dict[str, str] | None = None


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8").strip()


def read_int(path: Path) -> int:
    return int(read_text(path), 0)


def run(command: list[str], check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if check and result.returncode:
        raise TestFailure(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}"
        )
    return result


def parse_meminfo() -> dict[str, int]:
    wanted = {
        "MemTotal",
        "MemFree",
        "MemAvailable",
        "Active",
        "Inactive",
        "Unevictable",
        "Mlocked",
    }
    values: dict[str, int] = {}
    for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
        name, value = line.split(":", 1)
        if name in wanted:
            values[name] = int(value.split()[0])
    return values


def movable_zone() -> dict[str, int]:
    values: dict[str, int] = {}
    active = False
    wanted = {"pages free", "spanned", "present", "managed"}
    for line in Path("/proc/zoneinfo").read_text(encoding="utf-8").splitlines():
        if re.match(r"^Node \d+, zone\s+Movable$", line):
            active = True
            continue
        if active and re.match(r"^Node \d+, zone\s+", line):
            break
        if not active:
            continue
        stripped = line.strip()
        for key in wanted:
            if stripped.startswith(key):
                values[key.replace(" ", "_")] = int(stripped.split()[-1])
    return values


def dmesg_lines() -> list[str]:
    return run(["dmesg", "--color=never"]).stdout.splitlines()


def parse_result_line(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    if not path.exists():
        return result
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("RESULT "):
            continue
        for field in line.removeprefix("RESULT ").split():
            if "=" in field:
                key, value = field.split("=", 1)
                result[key] = value
    return result


def parse_marker_line(path: Path, marker: str) -> dict[str, str]:
    result: dict[str, str] = {}
    if not path.exists():
        return result
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith(marker + " "):
            continue
        for field in line.removeprefix(marker + " ").split():
            if "=" in field:
                key, value = field.split("=", 1)
                result[key] = value
    return result


class DonationTest:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.device = self.discover_device(args.device)
        self.uma = self.device / "uma"
        self.bdf = self.device.name
        self.block_size = read_int(self.uma / "donation_block_size_bytes")
        self.max_size = read_int(self.uma / "donatable_memory_bytes")
        self.range_start = read_int(self.uma / "donation_range_start")
        self.range_end = self.range_start + self.max_size
        self.card = self.discover_card()
        self.output = self.make_output_directory(args.output)
        self.workers: list[Worker] = []
        self.timed_out_writers: list[subprocess.Popen[str]] = []
        self.log_cursor = dmesg_lines()
        self.snapshots: list[dict[str, Any]] = []
        self.events: list[dict[str, Any]] = []
        self.baseline: dict[str, Any] | None = None
        self.failed = False
        self.cleanup_complete = False
        self.cleanup_authorized = False
        self.worker_oom_score_adj = str(
            max(0, read_int(Path("/proc/self/oom_score_adj")))
        )

        try:
            Path("/proc/self/oom_score_adj").write_text("-1000\n", encoding="ascii")
        except OSError:
            pass

    @staticmethod
    def discover_device(requested: str | None) -> Path:
        if requested:
            device = SYSFS_PCI / requested
            if not (device / "uma" / "donated_memory_bytes").exists():
                raise TestFailure(f"{requested} has no donation interface")
            return device

        matches = sorted(SYSFS_PCI.glob("*/uma/donated_memory_bytes"))
        if len(matches) != 1:
            raise TestFailure(
                f"expected exactly one donation interface, found {len(matches)}"
            )
        return matches[0].parents[1]

    def discover_card(self) -> Path:
        cards = sorted((self.device / "drm").glob("card[0-9]*"))
        if len(cards) != 1:
            raise TestFailure(f"expected one DRM card for {self.bdf}, found {cards}")
        return cards[0]

    def discover_render_node(self) -> Path:
        nodes = sorted((self.device / "drm").glob("renderD[0-9]*"))
        if len(nodes) != 1:
            raise TestFailure(
                f"expected one DRM render node for {self.bdf}, found {nodes}"
            )
        render_node = Path("/dev/dri") / nodes[0].name
        if not render_node.exists():
            raise TestFailure(f"DRM render node does not exist: {render_node}")
        return render_node

    @staticmethod
    def make_output_directory(requested: str | None) -> Path:
        if requested:
            output = Path(requested)
        else:
            stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            output = Path("/var/tmp") / f"amd-mem-donation-{stamp}"
        output.mkdir(parents=True, exist_ok=False)
        latest = Path("/var/tmp/amd-mem-donation-latest")
        try:
            latest.unlink(missing_ok=True)
            latest.symlink_to(output)
        except OSError:
            pass
        return output

    def event(self, kind: str, **fields: Any) -> None:
        event = {
            "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "kind": kind,
            **fields,
        }
        self.events.append(event)
        encoded = json.dumps(event, sort_keys=True)
        with (self.output / "events.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(encoded + "\n")
        print(encoded, flush=True)

    def state(self) -> dict[str, int]:
        names = (
            "donated_memory_bytes",
            "quarantined_memory_bytes",
            "donation_block_size_bytes",
            "donatable_memory_bytes",
            "donation_range_start",
        )
        return {name: read_int(self.uma / name) for name in names}

    def vram(self) -> dict[str, int]:
        result: dict[str, int] = {}
        device = self.card / "device"
        for path in device.glob("mem_info*"):
            if path.is_file():
                result[path.name] = read_int(path)
        return result

    def donation_blocks(self) -> list[dict[str, Any]]:
        blocks: list[dict[str, Any]] = []
        for path in SYSFS_MEMORY.glob("memory[0-9]*"):
            phys_index = path / "phys_index"
            if not phys_index.exists():
                continue
            index = int(read_text(phys_index), 16)
            address = index * self.block_size
            if not (self.range_start <= address < self.range_end):
                continue
            block: dict[str, Any] = {
                "name": path.name,
                "index": index,
                "address": address,
            }
            for name in ("state", "valid_zones", "removable"):
                child = path / name
                if child.exists():
                    block[name] = read_text(child)
            blocks.append(block)
        return sorted(blocks, key=lambda item: item["address"])

    def donation_resources(self) -> list[dict[str, int | str]]:
        resources: list[dict[str, int | str]] = []
        expression = re.compile(
            r"^\s*([0-9a-fA-F]+)-([0-9a-fA-F]+) : (.+)$"
        )
        for line in Path("/proc/iomem").read_text(encoding="ascii").splitlines():
            match = expression.match(line)
            if not match or match.group(3) != "System RAM (amdgpu)":
                continue
            start = int(match.group(1), 16)
            end = int(match.group(2), 16)
            if end < self.range_start or start >= self.range_end:
                continue
            resources.append({"start": start, "end": end, "name": match.group(3)})
        return resources

    def worker_residency(self) -> list[dict[str, Any]]:
        pids = {worker.process.pid: worker.name for worker in self.workers}
        if not pids:
            return []
        minor = self.card.name.removeprefix("card")
        path = Path("/sys/kernel/debug/dri") / minor / "amdgpu_gem_info"
        if not path.exists():
            return []

        records: list[dict[str, Any]] = []
        current_pid: int | None = None
        header = re.compile(r"^pid\s+(\d+)\s+command\s+(.+):$")
        bo = re.compile(
            r"^\s+0x([0-9a-fA-F]+):\s+(\d+) byte (VRAM|GTT)(.*)$"
        )
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = header.match(line)
            if match:
                current_pid = int(match.group(1))
                continue
            match = bo.match(line)
            if not match or current_pid not in pids:
                continue
            records.append(
                {
                    "worker": pids[current_pid],
                    "pid": current_pid,
                    "handle": int(match.group(1), 16),
                    "size": int(match.group(2)),
                    "domain": match.group(3),
                    "details": match.group(4).strip(),
                }
            )
        return records

    def snapshot(self, label: str) -> dict[str, Any]:
        snapshot = {
            "label": label,
            "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "kernel": os.uname().release,
            "bdf": self.bdf,
            "state": self.state(),
            "meminfo_kib": parse_meminfo(),
            "movable_zone": movable_zone(),
            "vram": self.vram(),
            "blocks": self.donation_blocks(),
            "resources": self.donation_resources(),
            "worker_residency": self.worker_residency(),
        }
        sequence = len(self.snapshots)
        path = self.output / f"{sequence:03d}-{label}.json"
        path.write_text(json.dumps(snapshot, indent=2, sort_keys=True) + "\n")
        self.snapshots.append(snapshot)
        return snapshot

    def kernel_log_delta(self, label: str) -> list[str]:
        current = dmesg_lines()
        if current[: len(self.log_cursor)] == self.log_cursor:
            delta = current[len(self.log_cursor) :]
        else:
            delta = current
            self.event("kernel_log_cursor_reset", label=label)
        self.log_cursor = current
        path = self.output / f"dmesg-{label}.log"
        path.write_text("\n".join(delta) + ("\n" if delta else ""))

        failures = [
            line
            for line in delta
            if any(pattern.search(line) for pattern in KERNEL_FAILURE_PATTERNS)
        ]
        if failures:
            raise TestFailure(
                f"kernel failure signature after {label}:\n" + "\n".join(failures)
            )
        return delta

    def validate_snapshot(
        self, snapshot: dict[str, Any], expected: int, exact_vram: bool
    ) -> None:
        state = snapshot["state"]
        if state["donated_memory_bytes"] != expected:
            raise TestFailure(
                f"donated size {state['donated_memory_bytes']} != {expected}"
            )
        if state["quarantined_memory_bytes"]:
            raise TestFailure(
                f"quarantined size is {state['quarantined_memory_bytes']}"
            )
        if expected % self.block_size:
            raise TestFailure(f"unaligned expected size {expected}")

        expected_addresses = set(
            range(self.range_end - expected, self.range_end, self.block_size)
        )
        actual_addresses = {item["address"] for item in snapshot["blocks"]}
        if actual_addresses != expected_addresses:
            raise TestFailure(
                f"hotplug blocks differ: expected={sorted(expected_addresses)} "
                f"actual={sorted(actual_addresses)}"
            )
        for block in snapshot["blocks"]:
            if block.get("state") != "online":
                raise TestFailure(f"{block['name']} is not online")
            if "Movable" not in block.get("valid_zones", ""):
                raise TestFailure(f"{block['name']} is not in ZONE_MOVABLE")
            if block.get("removable") != "1":
                raise TestFailure(f"{block['name']} is not removable")

        resources = snapshot["resources"]
        resource_addresses: set[int] = set()
        for resource in resources:
            start = int(resource["start"])
            end = int(resource["end"])
            size = end + 1 - start
            if start % self.block_size or size % self.block_size:
                raise TestFailure(f"mis-sized AMDGPU System RAM resource: {resource}")
            resource_addresses.update(
                range(start, end + 1, self.block_size)
            )
        if resource_addresses != expected_addresses:
            raise TestFailure(
                f"System RAM resources differ: expected={sorted(expected_addresses)} "
                f"actual={sorted(resource_addresses)}"
            )

        if self.baseline is not None:
            expected_memtotal = (
                self.baseline["meminfo_kib"]["MemTotal"] + expected // 1024
            )
            actual_memtotal = snapshot["meminfo_kib"]["MemTotal"]
            if actual_memtotal != expected_memtotal:
                raise TestFailure(
                    f"MemTotal {actual_memtotal} != {expected_memtotal} KiB"
                )

            baseline_vram = self.baseline["vram"].get("mem_info_vram_used")
            current_vram = snapshot["vram"].get("mem_info_vram_used")
            if current_vram is not None and current_vram < expected:
                raise TestFailure(
                    f"VRAM used {current_vram} is smaller than donated {expected}"
                )
            if exact_vram and baseline_vram is not None and current_vram is not None:
                baseline_nondonation = baseline_vram
                current_nondonation = current_vram - expected
                drift = abs(current_nondonation - baseline_nondonation)
                limit = self.args.vram_drift_mib * MIB
                if drift > limit:
                    fields = {
                        "label": snapshot["label"],
                        "drift": drift,
                        "limit": limit,
                        "vram_used": current_vram,
                        "donated": expected,
                        "baseline_vram_used": baseline_vram,
                    }
                    self.event("vram_drift", **fields)
                    if self.args.strict_vram_drift:
                        raise TestFailure(
                            "non-donation VRAM usage exceeded drift limit: "
                            + json.dumps(fields, sort_keys=True)
                        )

    def transition_writer_stuck(self) -> bool:
        pending: list[subprocess.Popen[str]] = []

        for process in self.timed_out_writers:
            if process.poll() is None:
                pending.append(process)
            else:
                process.communicate()
        self.timed_out_writers = pending
        return bool(pending)

    def write_target_value(self, target: int, label: str) -> None:
        path = self.uma / "donated_memory_bytes"
        command = [
            sys.executable,
            str(Path(__file__).resolve()),
            "--sysfs-write",
            str(path),
            str(target),
        ]
        process = subprocess.Popen(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        try:
            output, _ = process.communicate(timeout=self.args.transition_timeout)
        except subprocess.TimeoutExpired as error:
            process.kill()
            self.timed_out_writers.append(process)
            self.event(
                "target_timeout",
                label=label,
                target=target,
                timeout=self.args.transition_timeout,
                writer_pid=process.pid,
            )
            raise TestFailure(
                f"write target {target} exceeded "
                f"{self.args.transition_timeout:g}s timeout; "
                "refusing further ownership transitions while its writer "
                "remains in the kernel"
            ) from error

        if not process.returncode:
            return
        try:
            failure = json.loads(output)
            error_number = int(failure["errno"])
            error_text = str(failure["error"])
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            raise TestFailure(
                f"sysfs writer exited with {process.returncode}: {output.strip()}"
            ) from error
        raise OSError(error_number, error_text, path)

    def write_target(
        self, target: int, label: str, exact_vram: bool = True,
        allow_failure: bool = False,
    ) -> int:
        initial = read_int(self.uma / "donated_memory_bytes")
        self.event("target_begin", label=label, target=target, initial=initial)
        error: OSError | None = None
        try:
            self.write_target_value(target, label)
        except OSError as caught:
            error = caught

        actual = read_int(self.uma / "donated_memory_bytes")
        if error and not min(initial, target) <= actual <= max(initial, target):
            raise TestFailure(
                f"failed target {target} moved ownership in the wrong "
                f"direction: initial={initial} actual={actual}"
            )
        if error and actual % self.block_size:
            raise TestFailure(f"failed target left unaligned ownership: {actual}")
        snapshot = self.snapshot(label)
        self.validate_snapshot(snapshot, actual if error else target, exact_vram)
        self.kernel_log_delta(label)

        if error and not allow_failure:
            raise TestFailure(
                f"write target {target} failed after reaching {actual}: {error}"
            )
        self.event(
            "target_end",
            label=label,
            requested=target,
            actual=actual,
            error=str(error) if error else None,
        )
        return actual

    def target_value(self, token: str) -> int:
        normalized = token.strip().lower()
        if normalized in ("0", "zero"):
            return 0
        if normalized == "one":
            return self.block_size
        if normalized == "half":
            return (self.max_size // 2 // self.block_size) * self.block_size
        if normalized == "max":
            return self.max_size
        if normalized in ("max-one", "max_one"):
            return self.max_size - self.block_size
        match = re.fullmatch(r"(\d+)([mg])(?:i?b?)?", normalized)
        if not match:
            raise TestFailure(f"unknown target: {token}")
        scale = MIB if match.group(2) == "m" else GIB
        value = int(match.group(1)) * scale
        if value % self.block_size:
            raise TestFailure(f"target {token} is not block aligned")
        if value > self.max_size:
            raise TestFailure(f"target {token} exceeds maximum")
        return value

    def parse_targets(self) -> list[int]:
        if self.args.targets:
            tokens = self.args.targets.split(",")
        elif self.args.scenario == "smoke":
            tokens = ["one", "zero"]
        elif self.args.scenario == "staircase":
            tokens = [
                "one", "1G", "4G", "8G", "half", "24G", "28G",
                "max-one", "max", "28G", "24G", "half", "8G",
                "one", "zero",
            ]
        elif self.args.scenario == "concurrent":
            tokens = ["zero", "one", "1G", "4G", "8G"]
        else:
            tokens = ["28G"]
        return [self.target_value(token) for token in tokens]

    def helper_path(self, name: str) -> Path:
        requested = getattr(self.args, f"{name}_helper")
        default_name = {
            "cpu": "amd_mem_donation_cpu",
            "hip": "amd_mem_donation_hip",
            "drm": "amd_mem_donation_drm_hip",
            "userptr": "amd_mem_donation_userptr_hip",
        }[name]
        path = Path(requested) if requested else Path(__file__).with_name(default_name)
        if not path.is_file() or not os.access(path, os.X_OK):
            raise TestFailure(f"{name} helper is not executable: {path}")
        return path

    def spawn_worker(
        self, name: str, command: list[str], ready_marker: str
    ) -> Worker:
        path = self.output / f"{len(self.events):04d}-{name}.log"
        log_file = path.open("w", encoding="utf-8")
        launcher = [
            sys.executable,
            str(Path(__file__).resolve()),
            "--worker-exec",
            self.worker_oom_score_adj,
            *command,
        ]
        process = subprocess.Popen(
            launcher,
            text=True,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        worker = Worker(name, process, path, log_file, ready_marker)
        self.workers.append(worker)
        self.event("worker_start", name=name, pid=process.pid, command=command)
        return worker

    def wait_worker_ready(
        self, worker: Worker, timeout: float = 180.0
    ) -> dict[str, str]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            worker.log_file.flush()
            text = worker.log_path.read_text(encoding="utf-8", errors="replace")
            if worker.ready_marker in text:
                details = parse_marker_line(
                    worker.log_path, worker.ready_marker
                )
                worker.ready_details = details
                self.event(
                    "worker_ready",
                    name=worker.name,
                    details=details,
                )
                return details
            returncode = worker.process.poll()
            if returncode is not None:
                raise TestFailure(
                    f"{worker.name} exited before ready ({returncode}):\n{text}"
                )
            time.sleep(0.25)
        raise TestFailure(f"timeout waiting for {worker.name} readiness")

    @staticmethod
    def require_range_pages(worker: Worker) -> None:
        details = worker.ready_details or {}
        try:
            range_pages = int(details.get("range_pages", "0"), 0)
        except ValueError as error:
            raise TestFailure(
                f"{worker.name} reported invalid range_pages: {details}"
            ) from error
        if details.get("pfn_visible") != "1" or range_pages <= 0:
            raise TestFailure(
                f"{worker.name} did not allocate any observable pages from "
                f"donated System RAM: {details}"
            )

    def start_workers(
        self, duration: int, require_donated_pages: bool = False
    ) -> None:
        started: list[Worker] = []

        if self.args.cpu_mib:
            worker = self.spawn_worker(
                "cpu",
                [
                    str(self.helper_path("cpu")),
                    "--mib", str(self.args.cpu_mib),
                    "--seconds", str(duration),
                    "--range-start", hex(self.range_start),
                    "--range-size", str(self.max_size),
                    "--seed", str(self.args.seed),
                    "--wait-for-start",
                ],
                "CPU_READY",
            )
            self.wait_worker_ready(worker)
            started.append(worker)

        if self.args.gpu_mib:
            command = [
                str(self.helper_path("hip")),
                "--mib", str(self.args.gpu_mib),
                "--seconds", str(duration),
                "--chunk-mib", str(self.args.gpu_chunk_mib),
                "--pci-bdf", self.bdf,
                "--wait-for-start",
            ]
            if self.args.require_gpu_bytes:
                command.append("--require-bytes")
            worker = self.spawn_worker("gpu", command, "GPU_READY")
            self.wait_worker_ready(worker)
            started.append(worker)

        if self.args.drm_mib:
            render_node = self.discover_render_node()
            command = [
                str(self.helper_path("drm")),
                "--mib", str(self.args.drm_mib),
                "--seconds", str(duration),
                "--chunk-mib", str(self.args.drm_chunk_mib),
                "--domain", self.args.drm_domain,
                "--render-node", str(render_node),
                "--pci-bdf", self.bdf,
                "--range-start", hex(self.range_start),
                "--range-size", str(self.max_size),
                "--wait-for-start",
            ]
            if self.args.require_drm_bytes:
                command.append("--require-bytes")
            worker = self.spawn_worker("drm", command, "DRM_GPU_READY")
            self.wait_worker_ready(worker)
            started.append(worker)

        if self.args.userptr_mib:
            command = [
                str(self.helper_path("userptr")),
                "--mib", str(self.args.userptr_mib),
                "--seconds", str(duration),
                "--pci-bdf", self.bdf,
                "--range-start", hex(self.range_start),
                "--range-size", str(self.max_size),
                "--wait-for-start",
            ]
            worker = self.spawn_worker(
                "userptr", command, "USERPTR_GPU_READY"
            )
            self.wait_worker_ready(worker)
            started.append(worker)

        if require_donated_pages:
            for worker in started:
                if worker.name in ("cpu", "userptr"):
                    self.require_range_pages(worker)

        for worker in started:
            returncode = worker.process.poll()
            if returncode is not None:
                raise TestFailure(
                    f"{worker.name} exited before the common start "
                    f"({returncode}); see {worker.log_path}"
                )
        for worker in started:
            os.killpg(worker.process.pid, signal.SIGUSR1)
        self.event(
            "workers_released",
            duration=duration,
            workers=[worker.name for worker in started],
        )

    def hold_workers(self, seconds: int) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for worker in self.workers:
                returncode = worker.process.poll()
                if returncode not in (None, 0):
                    worker.log_file.flush()
                    raise TestFailure(
                        f"{worker.name} failed ({returncode}):\n"
                        f"{worker.log_path.read_text(errors='replace')}"
                    )
            time.sleep(min(1.0, max(0.0, deadline - time.monotonic())))

    def stop_workers(self) -> None:
        workers = list(self.workers)
        remaining: list[Worker] = []
        errors: list[str] = []

        def send_signal(worker: Worker, number: signal.Signals) -> None:
            try:
                os.killpg(worker.process.pid, number)
            except ProcessLookupError:
                pass
            except OSError as error:
                errors.append(
                    f"failed to send {number.name} to {worker.name}: {error}"
                )

        for worker in workers:
            if worker.process.poll() is None:
                send_signal(worker, signal.SIGINT)
        for worker in workers:
            returncode: int | None = None
            try:
                returncode = worker.process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                send_signal(worker, signal.SIGTERM)
                try:
                    returncode = worker.process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    send_signal(worker, signal.SIGKILL)
                    try:
                        returncode = worker.process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        errors.append(
                            f"{worker.name} could not be reaped after SIGKILL"
                        )
                        remaining.append(worker)
                        continue
            worker.log_file.flush()
            worker.log_file.close()
            result = parse_result_line(worker.log_path)
            self.event(
                "worker_end",
                name=worker.name,
                returncode=returncode,
                result=result,
                log=str(worker.log_path),
            )
            if returncode:
                errors.append(
                    f"{worker.name} exited with {returncode}; see {worker.log_path}"
                )
            if result.get("verify_errors") != "0":
                errors.append(f"{worker.name} integrity failure: {result}")

        self.workers = remaining
        if errors:
            raise TestFailure("; ".join(errors))

    def run_staircase(self, targets: list[int]) -> None:
        for cycle in range(self.args.cycles):
            for index, target in enumerate(targets):
                label = f"cycle{cycle:03d}-step{index:03d}-{target}"
                self.write_target(target, label)
                if self.args.hold_seconds:
                    time.sleep(self.args.hold_seconds)

    def run_donate_then_pressure(self, targets: list[int]) -> None:
        if (
            not self.args.cpu_mib
            and not self.args.gpu_mib
            and not self.args.drm_mib
            and not self.args.userptr_mib
        ):
            raise TestFailure("pressure scenario requires a CPU or GPU worker")
        for cycle in range(self.args.cycles):
            for index, target in enumerate(targets):
                label = f"cycle{cycle:03d}-pressure{index:03d}-{target}"
                self.write_target(target, f"{label}-donated")
                duration = self.args.work_seconds
                if self.args.return_under_pressure:
                    duration += 180
                self.start_workers(
                    duration, require_donated_pages=target > 0
                )
                self.snapshot(f"{label}-workers-ready")
                self.kernel_log_delta(f"{label}-workers-ready")
                if self.args.return_under_pressure:
                    self.write_target(
                        0,
                        f"{label}-live-return",
                        exact_vram=False,
                        allow_failure=True,
                    )
                self.hold_workers(self.args.work_seconds)
                self.stop_workers()
                self.snapshot(f"{label}-workers-stopped")
                self.kernel_log_delta(f"{label}-workers-stopped")
                self.write_target(0, f"{label}-returned")

    def run_pressure_then_donate(self, targets: list[int]) -> None:
        if (
            not self.args.cpu_mib
            and not self.args.gpu_mib
            and not self.args.drm_mib
            and not self.args.userptr_mib
        ):
            raise TestFailure("pressure scenario requires a CPU or GPU worker")
        for cycle in range(self.args.cycles):
            for index, target in enumerate(targets):
                label = f"cycle{cycle:03d}-prepressure{index:03d}-{target}"
                self.start_workers(self.args.work_seconds + 180)
                self.snapshot(f"{label}-workers-ready")
                self.write_target(
                    target,
                    f"{label}-transition",
                    exact_vram=False,
                    allow_failure=True,
                )
                self.hold_workers(self.args.work_seconds)
                self.stop_workers()
                self.write_target(0, f"{label}-returned")

    def run_random(self) -> None:
        generator = random.Random(self.args.seed)
        maximum = self.target_value(self.args.random_max)
        choices = list(range(0, maximum + self.block_size, self.block_size))
        for index in range(self.args.random_steps):
            target = generator.choice(choices)
            self.write_target(target, f"random{index:04d}-{target}")
            if self.args.hold_seconds:
                time.sleep(self.args.hold_seconds)
        self.write_target(0, "random-final-zero")

    def run_invalid(self) -> None:
        path = self.uma / "donated_memory_bytes"
        cases = (
            ("negative", "-1"),
            ("unaligned-one", "1"),
            ("unaligned-block", str(self.block_size - 1)),
            ("above-maximum", str(self.max_size + self.block_size)),
            ("overflow", str(1 << 65)),
            ("nonnumeric", "not-a-size"),
        )
        for label, value in cases:
            self.event("invalid_begin", label=label, value=value)
            try:
                path.write_text(value + "\n", encoding="ascii")
            except OSError as error:
                self.event(
                    "invalid_rejected", label=label, value=value, error=str(error)
                )
            else:
                raise TestFailure(f"invalid value {value!r} was accepted")
            snapshot = self.snapshot(f"invalid-{label}")
            self.validate_snapshot(snapshot, 0, exact_vram=True)
            self.kernel_log_delta(f"invalid-{label}")

    def run_concurrent(self, targets: list[int]) -> None:
        path = self.uma / "donated_memory_bytes"

        def write(value: int) -> str | None:
            try:
                self.write_target_value(value, f"concurrent-{value}")
            except (OSError, TestFailure) as error:
                return str(error)
            return None

        for cycle in range(self.args.cycles):
            self.event("concurrent_begin", cycle=cycle, targets=targets)
            with concurrent.futures.ThreadPoolExecutor(
                max_workers=len(targets)
            ) as executor:
                errors = list(executor.map(write, targets))
            if self.transition_writer_stuck():
                raise TestFailure(
                    "concurrent ownership writer timed out in the kernel"
                )
            actual = read_int(path)
            snapshot = self.snapshot(f"concurrent-{cycle:03d}-{actual}")
            self.validate_snapshot(snapshot, actual, exact_vram=False)
            self.kernel_log_delta(f"concurrent-{cycle:03d}-{actual}")
            self.event(
                "concurrent_end", cycle=cycle, actual=actual, errors=errors
            )
            if any(errors):
                raise TestFailure(f"concurrent valid writes failed: {errors}")
            if actual not in targets:
                raise TestFailure(
                    f"concurrent writes reached unsubmitted target {actual}; "
                    f"submitted={targets}"
                )
        self.write_target(0, "concurrent-final-zero")

    def run_state_protection(self) -> None:
        for cycle in range(self.args.cycles):
            self.write_target(
                self.block_size, f"state-protection-{cycle:03d}-donated"
            )
            blocks = self.donation_blocks()
            if len(blocks) != 1:
                raise TestFailure(
                    f"expected one donated memory block, found {blocks}"
                )

            state_path = SYSFS_MEMORY / str(blocks[0]["name"]) / "state"
            try:
                state_path.write_text("offline\n", encoding="ascii")
            except OSError as error:
                if error.errno != errno.EBUSY:
                    raise TestFailure(
                        f"direct {state_path} write failed with "
                        f"unexpected errno: {error}"
                    ) from error
                self.event(
                    "memory_block_state_rejected",
                    cycle=cycle,
                    block=blocks[0]["name"],
                    error=str(error),
                )
            else:
                raise TestFailure(
                    f"direct {state_path} offline operation was accepted"
                )

            snapshot = self.snapshot(
                f"state-protection-{cycle:03d}-rejected"
            )
            self.validate_snapshot(snapshot, self.block_size, exact_vram=True)
            self.kernel_log_delta(
                f"state-protection-{cycle:03d}-rejected"
            )
            self.write_target(
                0, f"state-protection-{cycle:03d}-returned"
            )

    def cleanup(self) -> None:
        errors: list[str] = []
        try:
            self.stop_workers()
        except Exception as error:  # cleanup must continue
            errors.append(str(error))

        if not self.cleanup_authorized:
            self.event(
                "cleanup_skipped",
                reason="test did not establish zero initial ownership",
            )
            if errors:
                raise TestFailure("; ".join(errors))
            return

        if self.transition_writer_stuck():
            errors.append(
                "a timed-out ownership writer remains in the kernel; "
                "refusing further sysfs access"
            )
            raise TestFailure("; ".join(errors))

        for attempt in range(30):
            state = self.state()
            if (
                state["donated_memory_bytes"] == 0
                and state["quarantined_memory_bytes"] == 0
            ):
                break
            try:
                self.write_target_value(0, f"cleanup-{attempt}")
            except (OSError, TestFailure) as error:
                errors.append(f"cleanup attempt {attempt}: {error}")
                if self.transition_writer_stuck():
                    break
            time.sleep(1)

        if self.transition_writer_stuck():
            errors.append(
                "cleanup writer remains in the kernel; final ownership "
                "state is unsafe to query"
            )
            raise TestFailure("; ".join(errors))

        state = self.state()
        if state["donated_memory_bytes"] or state["quarantined_memory_bytes"]:
            errors.append(f"cleanup left unsafe donation state: {state}")
        else:
            try:
                snapshot = self.snapshot("final-cleanup")
                self.validate_snapshot(snapshot, 0, exact_vram=True)
                self.kernel_log_delta("final-cleanup")
                self.cleanup_complete = True
            except Exception as error:
                errors.append(str(error))
        if errors:
            raise TestFailure("; ".join(errors))

    def execute(self) -> None:
        if os.geteuid() != 0:
            raise TestFailure("controller must run as root")
        if (
            ".amdmem" not in os.uname().release
            and not self.args.allow_non_test_kernel
        ):
            raise TestFailure(
                f"refusing non-test kernel {os.uname().release}; "
                "use --allow-non-test-kernel to override"
            )
        if self.block_size <= 0 or self.max_size <= 0:
            raise TestFailure("invalid donation geometry")
        if self.max_size % self.block_size or self.range_start % self.block_size:
            raise TestFailure("donation geometry is not block aligned")
        if self.args.cycles <= 0:
            raise TestFailure("cycles must be positive")
        if self.args.hold_seconds < 0 or self.args.work_seconds <= 0:
            raise TestFailure("test durations must be positive")
        if (
            self.args.cpu_mib < 0
            or self.args.gpu_mib < 0
            or self.args.drm_mib < 0
            or self.args.userptr_mib < 0
        ):
            raise TestFailure("worker allocation sizes cannot be negative")
        if self.args.vram_drift_mib < 0:
            raise TestFailure("VRAM drift tolerance cannot be negative")
        if (
            self.args.gpu_chunk_mib <= 0
            or self.args.drm_chunk_mib <= 0
            or self.args.random_steps <= 0
        ):
            raise TestFailure("chunk size and random steps must be positive")
        if self.args.transition_timeout <= 0:
            raise TestFailure("transition timeout must be positive")

        initial = self.state()
        if initial["donated_memory_bytes"] or initial["quarantined_memory_bytes"]:
            raise TestFailure(f"test must start from zero ownership: {initial}")
        self.cleanup_authorized = True

        self.baseline = self.snapshot("baseline")
        self.validate_snapshot(self.baseline, 0, exact_vram=True)
        (self.output / "dmesg-baseline.log").write_text(
            "\n".join(self.log_cursor) + "\n"
        )
        self.event(
            "test_begin",
            scenario=self.args.scenario,
            output=str(self.output),
            bdf=self.bdf,
            block_size=self.block_size,
            max_size=self.max_size,
            range_start=self.range_start,
        )

        targets = self.parse_targets()
        if self.args.scenario in ("smoke", "staircase"):
            self.run_staircase(targets)
        elif self.args.scenario == "donate-then-pressure":
            self.run_donate_then_pressure(targets)
        elif self.args.scenario == "pressure-then-donate":
            self.run_pressure_then_donate(targets)
        elif self.args.scenario == "random":
            self.run_random()
        elif self.args.scenario == "invalid":
            self.run_invalid()
        elif self.args.scenario == "concurrent":
            self.run_concurrent(targets)
        elif self.args.scenario == "state-protection":
            self.run_state_protection()
        else:
            raise TestFailure(f"unknown scenario {self.args.scenario}")

    def write_summary(self, result: str, error: str | None = None) -> None:
        summary = {
            "result": result,
            "error": error,
            "cleanup_complete": self.cleanup_complete,
            "output": str(self.output),
            "kernel": os.uname().release,
            "bdf": self.bdf,
            "block_size": self.block_size,
            "max_size": self.max_size,
            "range_start": self.range_start,
            "events": self.events,
            "snapshot_count": len(self.snapshots),
        }
        (self.output / "summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n"
        )


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scenario",
        choices=(
            "smoke",
            "staircase",
            "donate-then-pressure",
            "pressure-then-donate",
            "random",
            "invalid",
            "concurrent",
            "state-protection",
        ),
        default="smoke",
    )
    parser.add_argument("--device", help="PCI BDF, for example 0000:c4:00.0")
    parser.add_argument("--output", help="new result directory")
    parser.add_argument("--targets", help="comma-separated block-aligned targets")
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--hold-seconds", type=float, default=0.0)
    parser.add_argument("--work-seconds", type=int, default=10)
    parser.add_argument("--transition-timeout", type=float, default=300.0)
    parser.add_argument("--cpu-mib", type=int, default=0)
    parser.add_argument("--gpu-mib", type=int, default=0)
    parser.add_argument("--gpu-chunk-mib", type=int, default=256)
    parser.add_argument("--drm-mib", type=int, default=0)
    parser.add_argument("--drm-chunk-mib", type=int, default=256)
    parser.add_argument(
        "--drm-domain", choices=("vram", "gtt", "both"), default="vram"
    )
    parser.add_argument("--userptr-mib", type=int, default=0)
    parser.add_argument("--vram-drift-mib", type=int, default=64)
    parser.add_argument("--strict-vram-drift", action="store_true")
    parser.add_argument("--return-under-pressure", action="store_true")
    parser.add_argument("--cpu-helper")
    parser.add_argument("--hip-helper")
    parser.add_argument("--drm-helper")
    parser.add_argument("--userptr-helper")
    parser.add_argument("--require-gpu-bytes", action="store_true")
    parser.add_argument("--require-drm-bytes", action="store_true")
    parser.add_argument("--random-steps", type=int, default=100)
    parser.add_argument("--random-max", default="28G")
    parser.add_argument("--seed", type=int, default=0x616D646D656D)
    parser.add_argument("--allow-non-test-kernel", action="store_true")
    return parser


def sysfs_write_main(arguments: list[str]) -> int:
    if len(arguments) != 2:
        return 2
    path = Path(arguments[0])
    try:
        Path("/proc/self/oom_score_adj").write_text("0\n", encoding="ascii")
    except OSError:
        pass
    try:
        path.write_text(arguments[1] + "\n", encoding="ascii")
    except OSError as error:
        print(
            json.dumps(
                {
                    "errno": error.errno or errno.EIO,
                    "error": error.strerror or str(error),
                }
            ),
            flush=True,
        )
        return 1
    return 0


def worker_exec_main(arguments: list[str]) -> int:
    if len(arguments) < 2:
        return 2
    try:
        Path("/proc/self/oom_score_adj").write_text(
            arguments[0] + "\n", encoding="ascii"
        )
    except OSError as error:
        print(f"failed to make worker OOM-killable: {error}", file=sys.stderr)
        return 1
    os.execv(arguments[1], arguments[1:])
    return 127


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "--sysfs-write":
        return sysfs_write_main(sys.argv[2:])
    if len(sys.argv) >= 2 and sys.argv[1] == "--worker-exec":
        return worker_exec_main(sys.argv[2:])

    args = argument_parser().parse_args()
    test: DonationTest | None = None
    failure: str | None = None
    result = "FAIL"

    try:
        test = DonationTest(args)
        test.execute()
        result = "PASS"
    except (TestFailure, OSError, ValueError, KeyboardInterrupt) as error:
        failure = str(error)
        if test is not None:
            test.failed = True
            test.event("test_failure", error=failure)
        else:
            print(f"FAIL: {failure}", file=sys.stderr)
    finally:
        if test is not None:
            try:
                test.cleanup()
            except (TestFailure, OSError, ValueError) as cleanup_error:
                cleanup_message = f"cleanup failure: {cleanup_error}"
                failure = (
                    f"{failure}; {cleanup_message}" if failure else cleanup_message
                )
                result = "FAIL"
                test.event("cleanup_failure", error=str(cleanup_error))
            if failure is None:
                test.event("test_pass")
            test.write_summary(result, failure)
            print(f"{result}: results in {test.output}", flush=True)

    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
