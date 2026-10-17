# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""direct-scanout=no (vela.conf, applied by reload-config) keeps a fullscreen
dmabuf app out of direct scanout; "auto" lets it in (docs/configuration.md).

vela-slowgpu --fullscreen on a 512x320 output is an opaque dmabuf covering the
output 1:1: a scanout candidate. "state" tells whether the last frame was one."""

import os
import unittest

from harness import Session, tool

SIZE = "512x320@60"


def scanning(state):
    return state.output["scanout"]


class DirectScanout(unittest.TestCase):
    def open_app(self, vela):
        vela.open_window(command=[tool("vela-slowgpu"), "5", "--implicit", "--fullscreen"], timeout=15)

    def test_auto_goes_to_scanout(self):
        with Session(size=SIZE) as vela:
            self.open_app(vela)
            vela.wait_for(scanning, timeout=8, what="direct scanout")

    def test_no_keeps_compositing(self):
        with Session(size=SIZE) as vela:
            os.makedirs(os.path.dirname(vela.config_path()), exist_ok=True)
            with open(vela.config_path(), "w") as conf:
                conf.write("direct-scanout=no\n")
            vela.command("reload-config")
            self.open_app(vela)
            before = vela.wait_for(lambda s: s.output["frames"] > 20, timeout=8, what="frames")
            self.assertFalse(scanning(vela.state()))
            vela.wait_for(lambda s: s.output["frames"] > before.output["frames"] + 20, timeout=8, what="more frames")
            self.assertFalse(scanning(vela.state()))

    def test_reload_applies_live(self):
        with Session(size=SIZE) as vela:
            self.open_app(vela)
            vela.wait_for(scanning, timeout=8, what="direct scanout")
            os.makedirs(os.path.dirname(vela.config_path()), exist_ok=True)
            with open(vela.config_path(), "w") as conf:
                conf.write("direct-scanout=no\n")
            vela.command("reload-config")
            vela.wait_for(lambda s: not s.output["scanout"], timeout=8, what="back to compositing")

    def test_env_wins(self):
        with Session(size=SIZE, env={"VELA_SCANOUT": "0"}) as vela:
            self.open_app(vela)
            vela.wait_for(lambda s: s.output["frames"] > 20, timeout=8, what="frames")
            self.assertFalse(scanning(vela.state()))


if __name__ == "__main__":
    unittest.main()
