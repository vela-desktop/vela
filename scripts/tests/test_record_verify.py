"""Offline contracts for the GPU promo quality verifier."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "verify-promo.py"
spec = importlib.util.spec_from_file_location("vela_verify_promo", SCRIPT)
assert spec and spec.loader
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class VerifyPromoTests(unittest.TestCase):
    def test_black_frame_detector_tolerates_short_transitions(self):
        size = 64 * 36
        white, black = bytes([255]) * size, bytes(size)
        self.assertFalse(verify.consecutive_black_samples(white + black + white, size))
        self.assertTrue(verify.consecutive_black_samples(white + black + black, size))
        with self.assertRaisesRegex(ValueError, "truncated"):
            verify.consecutive_black_samples(white[:-1], size)

    def test_accepts_valid_video_and_rejects_tampering(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            name = "desktop"
            for ext in ("mp4", "webm"):
                (output / f"{name}.{ext}").write_bytes(b"mock-video")
            # Only the PNG signature and IHDR dimensions are inspected here;
            # FFmpeg decodes the real poster in hardware smoke runs.
            (output / f"{name}.png").write_bytes(
                b"\x89PNG\r\n\x1a\n" + b"\0" * 8 + struct.pack(">II", 640, 480))
            artifacts = {f"{name}.{ext}": verify.digest(output / f"{name}.{ext}")
                         for ext in ("mp4", "webm", "png")}
            manifest = {"scene": name, "capture_config": {"resolution": "640x480", "fps": 60,
                                                           "speed": 1.0},
                        "duration": 1.0, "frames_captured": 40, "frames_presented": 60,
                        "timeline": [{"at": 0.2, "ready_at": 0.25}],
                        "artifacts": artifacts}
            (output / f"{name}-manifest.json").write_text(json.dumps(manifest))

            def fake_run(args, **kwargs):
                if args[0] == "ffprobe":
                    return types.SimpleNamespace(stdout=json.dumps({
                        "streams": [{"width": 640, "height": 480,
                                     "r_frame_rate": "60/1"}],
                        "format": {"duration": "1.0"}}))
                return types.SimpleNamespace(stdout=bytes([128]) * 64 * 36 * 4)

            with mock.patch.object(verify.subprocess, "run", side_effect=fake_run):
                self.assertEqual(verify.inspect_clip(name, output)["scene"], name)
                (output / f"{name}.mp4").write_bytes(b"tampered")
                with self.assertRaisesRegex(ValueError, "corrupt"):
                    verify.inspect_clip(name, output)


if __name__ == "__main__":
    unittest.main()
