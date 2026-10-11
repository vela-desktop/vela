# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The audio test fixture itself (docs/audio.md §11, stage A0): the fake
devices, WirePlumber's policy on them, the fake app, levels, restarts and
isolation. Everything the library's tests (A1) will stand on."""

import os
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fixture import (HEADPHONES, MICROPHONE, SKIP, SPEAKERS, SURROUND, WIREPLUMBER_SAVE_DELAY,  # noqa: E402
                     AudioFixture, fake_stream_path, missing_tools)


def positions(node):
    """audio.position as a list: "[ FL FR ]" -> ["FL", "FR"]."""
    return node["info"]["props"]["audio.position"].strip("[] ").replace(",", " ").split()


class FixtureTest(unittest.TestCase):
    def setUp(self):
        self.audio = AudioFixture()
        self.audio.start()
        self.addCleanup(self.audio.stop)

    def test_devices_and_defaults(self):
        for name, media_class in ((SPEAKERS, "Audio/Sink"), (HEADPHONES, "Audio/Sink"),
                                  (SURROUND, "Audio/Sink"), (MICROPHONE, "Audio/Source/Virtual")):
            node = self.audio.node(name)
            self.assertIsNotNone(node, name)
            self.assertEqual(node["info"]["props"]["media.class"], media_class)
        self.assertEqual(self.audio.default("sink"), SPEAKERS)
        self.assertEqual(self.audio.default("source"), MICROPHONE)

    def test_channel_layouts(self):
        self.assertEqual(positions(self.audio.node(SPEAKERS)), ["FL", "FR"])
        self.assertEqual(positions(self.audio.node(SURROUND)), ["FL", "FR", "FC", "LFE", "RL", "RR"])
        self.assertEqual(positions(self.audio.node(MICROPHONE)), ["MONO"])

    def test_playback_follows_the_default_output(self):
        stream = self.audio.stream("--prop", "application.process.binary=vela-player",
                                   "--prop", "application.name=Vela Player")
        node = stream.wait_ready()
        stream.wait_state("streaming")
        props = self.audio.dump()
        info = next(o for o in props if o["id"] == node)["info"]["props"]
        self.assertEqual(info["media.class"], "Stream/Output/Audio")
        self.assertEqual(info["application.process.binary"], "vela-player")
        speakers = self.audio.node(SPEAKERS)["id"]
        self.audio.wait_for(lambda: (node, speakers) in self.audio.links(), "link to the speakers")

        # A new default: WirePlumber moves the stream there.
        self.audio.run("wpctl", "set-default", str(self.audio.node(HEADPHONES)["id"]))
        headphones = self.audio.node(HEADPHONES)["id"]
        self.audio.wait_for(lambda: (node, headphones) in self.audio.links(), "link to the headphones")

    def test_recording_uses_the_microphone(self):
        stream = self.audio.stream("--record", "--channels", "1",
                                   "--prop", "application.process.binary=vela-recorder")
        node = stream.wait_ready()
        stream.wait_state("streaming")
        microphone = self.audio.node(MICROPHONE)["id"]
        self.audio.wait_for(lambda: (microphone, node) in self.audio.links(), "link from the microphone")
        info = next(o for o in self.audio.dump() if o["id"] == node)["info"]["props"]
        self.assertEqual(info["media.class"], "Stream/Input/Audio")

    def test_levels_follow_amplitude_and_volume(self):
        player = self.audio.stream("--amplitude", "0.5")
        player.wait_state("streaming")
        meter = self.audio.stream("--record", "--prop", "stream.capture.sink=true",
                                  "--prop", f"target.object={SPEAKERS}", "--prop", "node.dont-reconnect=true")
        meter.wait_state("streaming")
        for left, right in meter.peaks(count=3)[1:]:
            self.assertAlmostEqual(left, 0.5, delta=0.01)
            self.assertAlmostEqual(right, 0.5, delta=0.01)

        # 50% on the cubic scale is 0.125 linear: the monitor hears 0.0625.
        self.audio.run("wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "0.5")
        self.audio.wait_for(lambda: abs(meter.peaks(count=2)[-1][0] - 0.0625) < 0.005, "the lower level")

    def test_pause_and_resume(self):
        stream = self.audio.stream()
        stream.wait_state("streaming")
        stream.send("pause")
        stream.wait_state("paused")
        stream.send("resume")
        stream.wait_state("streaming")

    def test_wireplumber_restart_keeps_its_state(self):
        self.audio.run("wpctl", "set-default", str(self.audio.node(HEADPHONES)["id"]))
        self.audio.wait_for(lambda: self.audio.default("sink") == HEADPHONES, "headphones as default")
        # WirePlumber saves its state about a second after a change, and not
        # when it exits: Vela waits as long before restarting it (§8). If a
        # new WirePlumber saves sooner, this still passes; if later, it fails.
        time.sleep(WIREPLUMBER_SAVE_DELAY)
        self.audio.restart_wireplumber()
        self.assertEqual(self.audio.default("sink"), HEADPHONES)
        # Remembered in the fixture's own state, nowhere else.
        state = self.audio.root / "state" / "wireplumber"
        self.assertTrue(any(state.iterdir()))

    def test_pipewire_restart(self):
        before = self.audio.node(SPEAKERS)
        self.audio.restart_pipewire()
        self.assertIsNotNone(self.audio.node(SPEAKERS))
        self.assertEqual(self.audio.default("sink"), SPEAKERS)
        self.assertIsNotNone(before)

    def test_isolation(self):
        env = self.audio.env
        root = str(self.audio.root)
        for key in ("HOME", "XDG_RUNTIME_DIR", "XDG_STATE_HOME", "XDG_CONFIG_HOME"):
            self.assertTrue(env[key].startswith(root), key)
        for key in ("PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS", "PULSE_SERVER"):
            self.assertNotIn(key, env)
        self.assertTrue(env["DBUS_SYSTEM_BUS_ADDRESS"].startswith(f"unix:path={root}"))
        self.assertEqual(len([line for line in self.audio.run("pw-cli", "ls", "Client").splitlines()
                              if "pipewire.access" in line and "unrestricted" not in line]), 0)

    def test_stop_cleans_up(self):
        stream = self.audio.stream()
        stream.wait_state("streaming")
        processes = [self.audio._pipewire, self.audio._wireplumber, stream.process]
        root = self.audio.root
        self.audio.stop()
        for process in processes:
            self.assertIsNotNone(process.poll())
        self.assertFalse(root.exists())

    def test_failed_stream_is_reported(self):
        # A crash or a sanitizer's report in the fake app fails the test.
        self.audio.stream("--no-such-option")
        with self.assertRaises(RuntimeError):
            self.audio.stop()


if __name__ == "__main__":
    missing = missing_tools()
    if not os.path.exists(fake_stream_path()):
        missing.append("vela-audio-fake-stream (build it, or set VELA_AUDIO_FAKE_STREAM)")
    if missing:
        print(f"skipped: {', '.join(missing)}", file=sys.stderr)
        sys.exit(SKIP)
    unittest.main(verbosity=2)
