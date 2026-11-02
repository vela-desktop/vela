# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("vast_runner", Path(__file__).parents[1] / "vast-runner.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def offer(**changes):
    base = {"id": 1, "num_gpus": 1, "gpu_name": "RTX 3060", "dph_total": 0.08,
            "verification": "verified", "reliability2": 0.99, "disk_space": 200,
            "cpu_cores_effective": 12, "cpu_ram": 32000, "gpu_ram": 12288,
            "inet_down": 500, "driver_version": "580.82.09"}
    return {**base, **changes}


class ChooseTests(unittest.TestCase):
    def test_cheapest_first(self):
        offers = [offer(id=1, dph_total=0.11), offer(id=2, dph_total=0.07), offer(id=3, dph_total=0.09)]
        self.assertEqual([o["id"] for o in m.choose(offers, 0.12)], [2, 3, 1])

    def test_only_one_gpu(self):
        # Cirun rented 2x and 4x offers for "1x": never again.
        self.assertEqual(m.choose([offer(num_gpus=2), offer(num_gpus=4)], 0.12), [])

    def test_price_cap(self):
        self.assertEqual(m.choose([offer(dph_total=0.121)], 0.12), [])
        self.assertEqual(len(m.choose([offer(dph_total=0.12)], 0.12)), 1)

    def test_rejects_what_cannot_run_the_suite(self):
        for bad in (offer(gpu_name="GTX 1080"),  # Pascal
                    offer(driver_version="550.120"),  # no Vulkan 1.4
                    offer(driver_version=""),
                    offer(cpu_cores_effective=1.5),
                    offer(cpu_ram=8000),
                    offer(gpu_ram=4096),
                    offer(disk_space=20),
                    offer(reliability2=0.95),
                    offer(verification="unverified"),
                    offer(inet_down=50)):
            with self.subTest(bad=bad):
                self.assertFalse(m.suitable(bad, 0.12))

    def test_chosen_gpu(self):
        offers = [offer(id=1, gpu_name="RTX 3060", dph_total=0.06), offer(id=2, gpu_name="RTX 4060")]
        self.assertEqual([o["id"] for o in m.choose(offers, 0.12, "RTX 4060")], [2])


if __name__ == "__main__":
    unittest.main()
