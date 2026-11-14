# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the isolated NVIDIA GPU rental and build sizing logic."""

import importlib.util
from pathlib import Path
import unittest
from unittest import mock


SCRIPTS = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


vast_runner = load("vast_runner_test", "vast-runner.py")
build_jobs = load("ci_nvidia_build_jobs_test", "ci-nvidia-build-jobs.py")


def offer(*, machine=1, gpu="RTX 3060", cpu=8, price=0.08, reliability=0.99,
          driver="595.99.01"):
    return {
        "id": machine, "machine_id": machine, "num_gpus": 1,
        "gpu_name": gpu, "cpu_cores_effective": cpu, "dph_total": price,
        "reliability2": reliability, "driver_version": driver,
        "verification": "verified", "disk_space": 50,
        "cpu_ram": 32000, "gpu_ram": 12000, "inet_down": 400,
    }


class FakeClock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class RentalSelectionTests(unittest.TestCase):
    def test_cores_and_reliability_can_beat_lowest_hourly_price(self):
        cheap = offer(machine=1, cpu=8, price=0.067, reliability=0.96)
        faster = offer(machine=2, cpu=16, price=0.080, reliability=0.995)
        self.assertEqual(
            vast_runner.choose([cheap, faster], 0.12)[0]["machine_id"], 2)

    def test_prefers_supported_recent_driver(self):
        older = offer(machine=1, cpu=24, price=0.050, driver="580.173")
        recent = offer(machine=2, cpu=8, price=0.095, driver="595.84")
        self.assertEqual(
            vast_runner.choose([older, recent], 0.12)[0]["machine_id"], 2)

    def test_enforces_price_cap_requested_gpu_and_skips_failed_machine(self):
        rejected = offer(machine=1, gpu="RTX 4070", price=0.125)
        good = offer(machine=2, gpu="RTX 4070", price=0.10)
        wrong = offer(machine=3, gpu="RTX 3060", price=0.05)
        self.assertEqual(
            vast_runner.choose([rejected, good, wrong], 0.12, gpu="RTX 4070"),
            [good])
        self.assertEqual(
            vast_runner.choose([good], 0.12, tried={2}), [])


class ProvisioningTests(unittest.TestCase):
    def probe(self, states, *, github_state="offline"):
        clock = FakeClock()
        state_iter = iter(states)
        last = states[-1]

        def vast_get(*_args):
            nonlocal last
            last = next(state_iter, last)
            return {"instances": [{"id": 123, **last}]}

        with mock.patch.object(vast_runner, "vast", side_effect=vast_get), \\
             mock.patch.object(vast_runner, "github", return_value={"status": github_state}), \\
             mock.patch.object(vast_runner.time, "monotonic", side_effect=clock.monotonic), \\
             mock.patch.object(vast_runner.time, "sleep", side_effect=clock.sleep):
            result = vast_runner.wait_online("vela-desktop", 9, 123)
        return result, clock.now

    def test_runner_already_online(self):
        result, elapsed = self.probe([{"actual_status": "loading", "status_msg": ""}],
                                     github_state="online")
        self.assertTrue(result)
        self.assertEqual(elapsed, 0)

    def test_fails_fast_on_dns_failure(self):
        result, elapsed = self.probe([
            {"actual_status": "loading", "status_msg": "Could not resolve host: cloud.vast.ai"}
        ])
        self.assertFalse(result)
        self.assertEqual(elapsed, 0)

    def test_stuck_image_pull_uses_image_timeout(self):
        result, elapsed = self.probe([
            {"actual_status": "loading", "status_msg": "Pulling layer"}
        ])
        self.assertFalse(result)
        self.assertGreaterEqual(elapsed, vast_runner.IMAGE_LOAD_TIMEOUT)
        self.assertLess(elapsed, 12 * 60)

    def test_running_container_uses_short_runner_timeout(self):
        result, elapsed = self.probe([
            {"actual_status": "running", "status_msg": "success, running image"}
        ])
        self.assertFalse(result)
        self.assertGreaterEqual(elapsed, vast_runner.RUNNER_READY_TIMEOUT)
        self.assertLess(elapsed, vast_runner.IMAGE_LOAD_TIMEOUT)

    def test_stopped_container_is_rejected_immediately(self):
        result, elapsed = self.probe([
            {"actual_status": "exited", "status_msg": "missing render node"}
        ])
        self.assertFalse(result)
        self.assertEqual(elapsed, 0)


class BuildSizingTests(unittest.TestCase):
    def test_respects_cpu_capacity(self):
        self.assertEqual(build_jobs.recommended_jobs(4, 64 * build_jobs.GIB), 4)

    def test_scales_above_old_eight_job_limit_with_ram(self):
        self.assertEqual(build_jobs.recommended_jobs(28, 16 * build_jobs.GIB), 14)

    def test_caps_job_count_and_preserves_ram(self):
        self.assertEqual(build_jobs.recommended_jobs(64, 128 * build_jobs.GIB), 16)
        self.assertEqual(build_jobs.recommended_jobs(64, 6 * build_jobs.GIB), 4)

    def test_memory_pressure_always_leaves_one_job(self):
        self.assertEqual(build_jobs.recommended_jobs(32, 0), 1)

    def test_v2_cgroup_cpu_quota(self):
        def read_text(path, encoding="ascii"):
            if str(path).endswith("/cpu.max"):
                return "1050000 100000"
            raise FileNotFoundError(path)

        with mock.patch.object(build_jobs.Path, "read_text", autospec=True,
                               side_effect=read_text):
            self.assertEqual(build_jobs.cgroup_cpu_quota(), 10)


if __name__ == "__main__":
    unittest.main()
