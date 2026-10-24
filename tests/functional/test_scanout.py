# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""direct-scanout=no (vela.conf, applied by reload-config) keeps a fullscreen
dmabuf app out of direct scanout; "auto" lets it in (docs/configuration.md).

vela-slowgpu --fullscreen on a 512x320 output is an opaque dmabuf covering the
output 1:1: a scanout candidate. "state" tells whether the last frame was one."""

import os
import signal
import tempfile
import os.path
import unittest

from harness import Session, tool

SIZE = "512x320@60"


def scanning(state):
    return state.output["scanout"]


class DirectScanout(unittest.TestCase):
    def open_app(self, vela):
        vela.open_window(command=[tool("vela-slowgpu"), "5", "--implicit", "--fullscreen"], timeout=15)

    def configure(self, vela, mode):
        os.makedirs(os.path.dirname(vela.config_path()), exist_ok=True)
        with open(vela.config_path(), "w") as conf:
            conf.write(f"direct-scanout={mode}\n")
        vela.command("reload-config")

    def test_auto_goes_to_scanout(self):
        with Session(size=SIZE, env={"VELA_SCANOUT": "1"}) as vela:
            self.open_app(vela)
            vela.wait_for(scanning, timeout=8, what="direct scanout")

    def test_no_keeps_compositing(self):
        with Session(size=SIZE, env={"VELA_SCANOUT": "1"}) as vela:
            self.configure(vela, "no")
            self.open_app(vela)
            before = vela.wait_for(lambda s: s.output["frames"] > 20, timeout=8, what="frames")
            self.assertFalse(scanning(vela.state()))
            vela.wait_for(lambda s: s.output["frames"] > before.output["frames"] + 20, timeout=8, what="more frames")
            self.assertFalse(scanning(vela.state()))

    def test_reload_applies_live(self):
        with Session(size=SIZE, env={"VELA_SCANOUT": "1"}) as vela:
            self.open_app(vela)
            before = vela.wait_for(scanning, timeout=8, what="direct scanout")
            self.configure(vela, "no")
            # The state flag must not flip until a *new* composed buffer has
            # been committed. Otherwise a failed commit gives a false pass.
            composed = vela.wait_for(
                lambda s: s.output["frames"] > before.output["frames"] and not scanning(s),
                timeout=8, what="a successfully committed composited frame")
            self.configure(vela, "auto")
            vela.wait_for(
                lambda s: s.output["frames"] > composed.output["frames"] and scanning(s),
                timeout=8, what="direct scanout restored by reload-config")

    def test_reload_from_scanout_with_idle_client(self):
        with Session(size=SIZE, env={"VELA_SCANOUT": "1"}) as vela:
            self.open_app(vela)
            vela.wait_for(scanning, timeout=8, what="direct scanout")
            # Stop the client without unmapping it. A settings reload must
            # still force a replacement frame even without any new commits.
            client = vela.clients[-1]
            client.send_signal(signal.SIGSTOP)
            try:
                before = vela.state().output["frames"]
                self.configure(vela, "no")
                vela.wait_for(
                    lambda s: s.output["frames"] > before and not scanning(s),
                    timeout=8, what="composited frame with idle fullscreen client")
            finally:
                client.send_signal(signal.SIGCONT)


    def test_scanout_feedback_cleared_immediately_on_disable(self):
        with Session(size=SIZE, env={"VELA_SCANOUT": "1"}) as vela:
            self.open_app(vela)
            vela.wait_for(scanning, timeout=8, what="direct scanout")
            # DMA-BUF feedback is deliberately debounced for normal window
            # changes. Once disabled, even a stopped client needs its default
            # feedback restored without waiting another 30 frames.
            vela.wait_for(lambda s: s.output["scanoutFeedback"], timeout=12,
                          what="scanout DMA-BUF feedback")
            client = vela.clients[-1]
            client.send_signal(signal.SIGSTOP)
            try:
                self.configure(vela, "no")
                state = vela.wait_for(
                    lambda s: not s.output["scanout"] and not s.output["scanoutFeedback"],
                    timeout=8, what="composited output with default DMA-BUF feedback")
                self.assertFalse(state.output["deliveredScanout"])
            finally:
                client.send_signal(signal.SIGCONT)

    def test_failed_composite_commit_preserves_last_scanout(self):
        # Headless-only commit fault injection: no hardware driver manipulation
        # and no effect on ordinary sessions. Keep the failed-commit marker
        # present during the auto -> no transition.
        with tempfile.TemporaryDirectory() as tmp:
            fault = os.path.join(tmp, "fail-commit")
            with Session(size=SIZE, env={
                    "VELA_TEST_FAULTS": "1",
                    "VELA_SCANOUT": "1",
                    "VELA_TEST_FAIL_COMPOSE_COMMIT_FILE": fault}) as vela:
                self.open_app(vela)
                vela.wait_for(scanning, timeout=8, what="initial scanout")
                client = vela.clients[-1]
                client.send_signal(signal.SIGSTOP)
                try:
                    with open(fault, "w") as marker:
                        marker.write("fail")
                    before = vela.state().output
                    self.configure(vela, "no")
                    vela.wait_for_log("injected composed commit failure", timeout=8)
                    failed = vela.state().output
                    self.assertTrue(failed["scanout"])
                    self.assertTrue(failed["deliveredScanout"])
                    self.assertEqual(failed["tearing"], before["tearing"])
                    self.assertEqual(failed["deliveredTearing"], before["deliveredTearing"])
                    os.unlink(fault)
                    vela.command("reload-config")
                    vela.wait_for(
                        lambda s: not s.output["scanout"] and not s.output["deliveredScanout"],
                        timeout=8, what="composited recovery after injected commit failure")
                finally:
                    client.send_signal(signal.SIGCONT)

    def test_failed_composite_then_successful_mode_fallback(self):
        # A mode update can commit via output.c after the composed buffer
        # fails. That successful external commit must clear the old state.
        with tempfile.TemporaryDirectory() as tmp:
            fault = os.path.join(tmp, "fail-commit")
            with Session(size=SIZE, env={
                    "VELA_TEST_FAULTS": "1",
                    "VELA_SCANOUT": "1",
                    "VELA_TEST_FAIL_COMPOSE_COMMIT_FILE": fault}) as vela:
                self.open_app(vela)
                vela.wait_for(scanning, timeout=8, what="initial scanout")
                with open(fault, "w") as marker:
                    marker.write("fail")
                try:
                    vela.randr("--output", "HEADLESS-1", "--scale", "1.25")
                    state = vela.wait_for(lambda s: s.output["scale"] == 1.25,
                                          timeout=8, what="fallback scale committed")
                    self.assertFalse(state.output["scanout"])
                    self.assertFalse(state.output["deliveredScanout"])
                    self.assertFalse(state.output["tearing"])
                    self.assertFalse(state.output["deliveredTearing"])
                    self.assertIn("injected composed commit failure", vela.log_text())
                finally:
                    os.unlink(fault)

    def test_yes_alias_keeps_scanout_enabled(self):
        with Session(size=SIZE, env={"VELA_SCANOUT": "1"}) as vela:
            self.configure(vela, "yes")
            self.open_app(vela)
            vela.wait_for(scanning, timeout=8, what="legacy yes scanout")

    def test_env_wins(self):
        with Session(size=SIZE, env={"VELA_SCANOUT": "0"}) as vela:
            self.open_app(vela)
            self.assertTrue(vela.query("scanout-policy")["envDisabled"])
            vela.wait_for(lambda s: s.output["frames"] > 20, timeout=8, what="frames")
            self.assertFalse(scanning(vela.state()))


if __name__ == "__main__":
    unittest.main()
