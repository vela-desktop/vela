# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Real protocol traffic on an isolated headless compositor, not a DRM benchmark."""

import json
import os
import shutil
import subprocess
from pathlib import Path
import sys
import tempfile
import time
import unittest

from harness import Session, tool

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
from vela_bench.diagnostics import parse_probe
from vela_bench.conditions import compare_outputs, desired_profile, display_command, normalized_outputs
from vela_bench.host import Layout
from vela_bench.measurements import motion_metrics, producer_metrics
from vela_bench.calibration import Lines, run_input, stop_process
from vela_bench.measurements import input_metrics


class BenchmarkPresentationProbe(unittest.TestCase):
    def test_p3_native_scene_is_identified_in_output_and_moves_as_a_real_window(self):
        if not all(Path(tool(name)).is_file() for name in ('vela-bench-scene','vela-bench-desktop')):
            self.skipTest('optional native Qt desktop instruments unavailable')
        from vela_bench.desktop import SCENE, LAUNCHED, DRAG_GRIP, changed
        with Session(size="1280x720@60") as session, tempfile.TemporaryDirectory() as tmp:
            env = dict(session.client_env, QT_QPA_PLATFORM='wayland')
            process = subprocess.Popen([tool('vela-bench-scene'),'--scene'], env=env,
                stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
            self.addCleanup(stop_process,process)
            lines=Lines(process)
            self.assertEqual(lines.next(time.monotonic()+5)['event'],'scene-ready')
            def capture(name, *, cursor=False):
                image=str(Path(tmp)/name)
                session.screenshot(image,cursor=cursor)
                done=subprocess.run([tool('vela-bench-desktop'),'--analyze',image],env=env,
                                    capture_output=True,text=True,timeout=5)
                self.assertEqual(done.returncode,0,done.stderr)
                data=json.loads(done.stdout)
                return data, next(m for m in data['markers'] if m['frame']==SCENE)
            time.sleep(.3)
            before,origin=capture('before.png')
            self.assertEqual(len(before['tiles']),576)
            x,y=origin['x']+DRAG_GRIP[0],origin['y']+DRAG_GRIP[1]
            # Capture while held, including the cursor. The former test only
            # captured after release and missed corruption of the marker bits.
            pointer = subprocess.Popen([tool('vela-input'),'move',str(x),str(y),'sleep','100',
                'down','left','sleep','1000','move',str(x+80),str(y),'sleep','1000',
                'move',str(x+160),str(y),'sleep','1000','up','left','sleep','100'],
                env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
            self.addCleanup(stop_process,pointer)
            self.assertEqual(lines.next(time.monotonic()+5),{'event':'move-request','accepted':True})
            positions=[]
            for index in range(3):
                _, visible=capture(f'held-{index}.png',cursor=True)
                positions.append(visible['x'])
                if index<2: time.sleep(1.05)
            self.assertEqual(len(set(positions)),3)
            self.assertGreaterEqual(positions[-1]-positions[0],150)
            self.assertEqual(pointer.wait(timeout=3),0)
            after,final=capture('after.png')
            self.assertGreaterEqual(final['x']-origin['x'],150)
            self.assertGreater(changed(before['tiles'],after['tiles']),12)
            # Fixture receipt and distinct marker; the native launcher itself
            # still requires real-shell TTY acceptance, not this direct exec.
            launched=subprocess.Popen([tool('vela-bench-scene'),'--launch'],env=env,
                stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
            self.addCleanup(stop_process,launched)
            Lines(launched).next(time.monotonic()+5)
            receipt=subprocess.run([tool('vela-bench-scene'),'--receipt'],env=env,
                                   capture_output=True,text=True,timeout=5)
            self.assertEqual(receipt.returncode,0,receipt.stderr)
            self.assertEqual(json.loads(receipt.stdout)['marker'],LAUNCHED)

    def test_p2_full_input_controller_drains_105_native_actions(self):
        # Exercise the production pipe controller with a debug-input fixture.
        # This deliberately does not qualify kernel uinput or physical latency.
        script = '''import json,sys,subprocess,time,signal
anchor=subprocess.Popen([sys.argv[1],'sleep','60000'],stdout=subprocess.DEVNULL)
def stop(signum,frame):raise SystemExit(0)
signal.signal(signal.SIGTERM,stop)
try:
    time.sleep(.1)
    print(json.dumps({'event':'uinput-ready','method':'linux-uinput-f24-v1','clock':'CLOCK_MONOTONIC','sysfs':'/sys/devices/virtual/input/input999999999'}),flush=True)
    for index,line in enumerate(sys.stdin,1):
        before=time.monotonic_ns()
        subprocess.run([sys.argv[1],'keycode','194'],check=True,stdout=subprocess.DEVNULL)
        after=time.monotonic_ns()
        print(json.dumps({'event':'injected','action_id':index,'before_monotonic_ns':before,'after_monotonic_ns':after}),flush=True)
finally:
    anchor.terminate()
    anchor.wait(timeout=3)
'''
        with Session(size="1280x720@60") as session, tempfile.TemporaryDirectory() as tmp:
            from types import SimpleNamespace
            layout = Layout(Path(tmp))
            source = Path(tmp)
            class Host:
                def spawn(self, command, errors):
                    command = [sys.executable, '-c', script, tool('vela-input')] if command[0].endswith('vela-bench-input') else [tool('vela-bench-probe'), *command[command.index(str(layout.probe))+1:]]
                    return subprocess.Popen(command, env=session.client_env, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=errors, bufsize=0)
                def run(self, command, **kwargs):
                    return SimpleNamespace(returncode=0, stdout='', stderr='')
            result = run_input(Host(), {'uid': os.getuid(), 'gid': os.getgid()}, source, 'vela', layout,
                               lambda: None, measurement_name='measure-input')
            self.assertEqual(result['status'], 'passed', result.get('error'))
            self.assertEqual(result['metrics']['actions'], 105)
            rows = [json.loads(line) for line in (source/'vela-measure-input.jsonl').read_text().splitlines()]
            injections = json.loads((source/'vela-measure-input.injections.json').read_text())
            self.assertEqual(input_metrics(rows, injections)['actions'], 100)

    def test_p2_complete_motion_budget_uses_real_feedback(self):
        with Session(size="1280x720@60") as session:
            result = subprocess.run([tool("vela-bench-probe"), "--frames", "630", "--timeout", "25"],
                env=session.client_env, text=True, capture_output=True, timeout=28)
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            metrics = motion_metrics(result.stdout, rows, {"mode": {"refresh_mhz": 60000}}, physical_output_required=False)
            self.assertEqual(metrics["submitted"], 600)
            self.assertEqual(metrics["presented"] + metrics["discarded"], 600)
            self.assertEqual(metrics["warmup_frames"], 30)
            self.assertFalse(metrics["physical_output_verified"])

    def test_p2_gpu_producer_reports_achieved_delay_and_visible_frames(self):
        if not Path(tool("vela-slowgpu")).is_file():
            self.skipTest("optional Vulkan/GBM producer unavailable")
        for mode in ("explicit", "implicit"):
            with self.subTest(mode=mode), Session(size="1280x720@60") as session:
                process = subprocess.Popen([tool("vela-slowgpu"), "150", "--json", "--until-stdin-eof",
                    *(["--implicit"] if mode == "implicit" else [])], env=session.client_env,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
                self.addCleanup(self.stop, process)
                # Bounded complete fixture, no input, seat or physical output changes.
                lines = Lines(process)
                while lines.next(time.monotonic() + 10).get('event') != 'producer-presented':
                    pass
                begin = time.monotonic_ns()
                foreground = subprocess.run([tool('vela-bench-probe'), '--frames', '120', '--timeout', '8'],
                    env=session.client_env, capture_output=True, timeout=10)
                self.assertEqual(foreground.returncode, 0, foreground.stderr.decode())
                end = time.monotonic_ns()
                time.sleep(.3)
                process.stdin.close()
                process.stdin = None
                output, errors = process.communicate(timeout=8)
                self.assertEqual(process.returncode, 0, errors.decode() + output.decode())
                output = ''.join(json.dumps(row)+'\n' for row in lines.records).encode() + lines.pending + output
                rows = [json.loads(line) for line in output.splitlines()]
                calibration = next(row for row in rows if row.get('event') == 'calibrated')
                self.assertEqual(calibration['dispatches'], 50)
                self.assertGreaterEqual(calibration['iterations'], calibration['dispatches'])
                visible = [r for r in rows if r.get("event") == "producer-presented"]
                self.assertGreaterEqual(len(visible), 5)
                window = [begin, end]
                metrics = producer_metrics(rows, mode, window, physical_output_required=False)
                self.assertTrue(metrics["measurement_window_covered"])
                self.assertTrue(metrics["visible_updates_verified"])
                self.assertFalse(metrics["physical_output_verified"])

    def stop(self, process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
    def probe(self, session, frames, delay=0):
        result = subprocess.run([tool("vela-bench-probe"), "--frames", str(frames), "--delay-ms", str(delay),
                                 "--timeout", "5"], env=session.client_env, text=True, capture_output=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr + "\n" + result.stdout)
        metrics = parse_probe(result.stdout, frames=frames, delay=delay)
        self.assertEqual(metrics["frames"], frames)
        self.assertEqual(metrics["scope"], "diagnostic-client-only")
        records = [json.loads(line) for line in result.stdout.splitlines()]
        summary = records[-1]
        metadata = next(row for row in records if row["event"] == "probe")
        self.assertEqual(metadata["display_protocol"], "wayland")
        self.assertEqual(metadata["window_protocol"], "xdg-shell")
        self.assertEqual(metadata["buffer_backend"], "wl_shm")
        self.assertEqual(summary["event"], "summary")
        self.assertTrue(summary["complete"])
        self.assertEqual(summary["committed"], frames)
        self.assertEqual(summary["presented"] + summary["discarded"], frames)
        self.assertGreater(summary["presented"], 0)
        commits = [x for x in records if x["event"] == "commit"]
        terminal = [x for x in records if x["event"] in ("presented", "discarded")]
        self.assertEqual([x["frame"] for x in commits], list(range(1, frames + 1)))
        self.assertEqual(sorted(x["frame"] for x in terminal), list(range(1, frames + 1)))
        for row in terminal:
            if row["event"] == "presented":
                self.assertGreaterEqual(row["presentation_ns"], row["commit_clock_ns"])
        return records

    def test_each_submission_has_matching_feedback_and_clock_identity(self):
        with Session(size="800x600@60") as session:
            records = self.probe(session, 12)
            clocks = [x for x in records if x["event"] == "clock"]
            self.assertEqual(len(clocks), 1)
            self.assertIsInstance(clocks[0]["presentation_clock_id"], int)
            commits = [x["commit_clock_ns"] for x in records if x["event"] == "commit"]
            self.assertTrue(all(a < b for a, b in zip(commits, commits[1:])))

    def test_known_client_delay_is_recorded_without_relabeling_it_gpu_delay(self):
        with Session(size="800x600@60") as session:
            records = self.probe(session, 3, delay=150)
            metadata = next(x for x in records if x["event"] == "probe")
            self.assertEqual(metadata["scope"], "diagnostic-client-only")
            self.assertEqual(metadata["delay_requested_ms"], 150)
            delays = [x["delay_elapsed_ns"] for x in records if x["event"] == "commit"]
            self.assertEqual(len(delays), 3)
            self.assertTrue(all(value >= 150_000_000 for value in delays))

    def test_input_fixture_maps_raw_f24_to_distinct_presented_frames(self):
        # This checks the client with Vela's headless debug transport. It is
        # deliberately not a claim that the real uinput/DRM path is qualified.
        with Session(size="800x600@60") as session:
            probe = subprocess.Popen([tool("vela-bench-probe"), "--inputs", "3", "--timeout", "6"],
                env=session.client_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.addCleanup(self.stop, probe)
            sender = subprocess.Popen([tool("vela-input"), "sleep", "500", "keycode", "194", "sleep", "150",
                "keycode", "194", "sleep", "150", "keycode", "194", "sleep", "100"],
                env=session.client_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.addCleanup(self.stop, sender)
            output, errors = probe.communicate(timeout=9)
            _, sender_errors = sender.communicate(timeout=3)
            self.assertEqual(sender.returncode, 0, sender_errors)
            self.assertEqual(probe.returncode, 0, errors + output)
            rows = [json.loads(line) for line in output.splitlines()]
            self.assertEqual([row["action_id"] for row in rows if row["event"] == "input"], [1, 2, 3])
            self.assertEqual([row["input_id"] for row in rows if row["event"] == "commit"], [0, 1, 2, 3])
            filtered = "".join(json.dumps(row) + "\n" for row in rows if row["event"] not in ("input", "input-ready"))
            parse_probe(filtered, frames=4, method="wayland-input-presentation-v1", scope="software-input-to-reported-presentation")

    def test_output_adapter_readback_and_captured_visual_id(self):
        if not shutil.which("wlr-randr") or not Path(tool("vela-bench-desktop")).is_file():
            self.skipTest("optional wlr-randr/libkscreen adapter unavailable")
        with Session(size="800x600@60") as session:
            inventory = subprocess.run(["/usr/bin/wlr-randr", "--json"], env=session.client_env,
                                       check=True, capture_output=True, text=True, timeout=5)
            outputs = normalized_outputs(inventory.stdout, "vela")
            profile = desired_profile(outputs, outputs[0]["name"], {"width": 800, "height": 600, "refresh_mhz": 60000})
            subprocess.run(display_command(Layout(), "vela", profile["outputs"]), env=session.client_env,
                           check=True, capture_output=True, timeout=5)
            observed = subprocess.run(["/usr/bin/wlr-randr", "--json"], env=session.client_env,
                                     check=True, capture_output=True, text=True, timeout=5)
            compare_outputs(normalized_outputs(observed.stdout, "vela"), profile["outputs"])
            probe = subprocess.Popen([tool("vela-bench-probe"), "--frames", "120", "--delay-ms", "40", "--timeout", "10"],
                env=session.client_env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            self.addCleanup(self.stop, probe)
            time.sleep(.3)
            with tempfile.TemporaryDirectory() as directory:
                image = str(Path(directory) / "capture.png")
                subprocess.run([tool("vela-screen-dump"), image], env=session.client_env,
                               check=True, capture_output=True, timeout=5)
                decoded = subprocess.run([tool("vela-bench-desktop"), "--analyze", image],
                                         check=True, capture_output=True, text=True, timeout=5)
                data = json.loads(decoded.stdout)
                self.assertEqual((data["width"], data["height"]), (800, 600))
                self.assertEqual(len(data["markers"]), 1)
                self.assertGreater(data["markers"][0]["frame"], 0)


if __name__ == "__main__":
    unittest.main()
