#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The pictures Settings > Personalization shows (themes/screenshots/): every
theme and every surface style, as the real shell draws it, on a headless Vela
with neutral data (recording_core.Desktop: a made-up user, demo apps, private
D-Bus and PipeWire, English text). Not drawn by hand, and never an author's
screenshot (docs/theme-system.md, "Preview and user interaction").

  themes/screenshots/themes/<theme>.png              the desktop in the theme
  themes/screenshots/themes/<theme>-<surface>.png    a surface as it draws it
  themes/screenshots/<surface>/<style>.png           a style
  (-qs.png and -nc.png: quick settings and the notification center)

Usage: scripts/theme-pictures.py [--build build] [--only taskbar|startmenu|notifications|themes]
Run it again after changing a theme, a style or what they draw: it rewrites
every picture and removes those of styles that no longer exist.
"""

import argparse
import os
from pathlib import Path
import sys
import time

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import recording_core as core  # noqa: E402

THEMES = ROOT / "themes"
PICTURES = THEMES / "screenshots"
SURFACES = ("taskbar", "startmenu", "notifications")
# (key, layer namespace, picture suffix) of each surface's panels.
PANELS = {"startmenu": (("super", "vela-start-menu", ""),),
          "notifications": (("super+a", "vela-quick-settings", "-qs"),
                            ("super+n", "vela-notification-center", "-nc"))}
# A taskbar's picture: the bottom of a 1280 x 720 desktop, the bar with the
# wallpaper above it, so a floating bar shows that it floats (StyleGallery.qml
# draws it this shape).
TASKBAR_SIZE = "1280x720@60"
TASKBAR_STRIP = 96
# A theme's picture: the whole desktop with a window, a third of 1920 x 1080.
THEME_SIZE = (640, 360)


class Still(core.Scene):
    name = "theme-pictures"

    def __init__(self, size):
        self.size = size


def names(directory):
    return sorted(path.stem for path in directory.glob("*.conf"))


def link_sources(desktop):
    """This checkout's themes and styles in the session's own
    ~/.config/vela/themes/, which wins over an installed Vela's."""
    target = Path(desktop.vela.env["XDG_CONFIG_HOME"]) / "vela" / "themes"
    target.mkdir(parents=True, exist_ok=True)
    for theme in THEMES.glob("*.conf"):
        (target / theme.name).symlink_to(theme)
    for surface in SURFACES:
        (target / surface).symlink_to(THEMES / surface)


def use(desktop, theme="vela", surface=None, style=None):
    """theme= and one surface's style in vela.conf; the shell takes them."""
    path = Path(desktop.vela.config_path())
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"theme={theme}"]
    if surface:
        lines.append(f"{surface}-style={style}")
    path.write_text("\n".join(lines) + "\n")
    desktop.vela.command("reload-config")
    time.sleep(1.5) # the shell reads the file, the bar and its blur settle


def shoot(desktop, path, region=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    desktop.vela.screenshot(str(path), region=region)
    with Image.open(path) as image:
        image.load()
    image.save(path, optimize=True)


def shoot_taskbar(desktop, path):
    output = desktop.vela.state()["outputs"][0]
    shoot(desktop, path, (0, output["h"] - TASKBAR_STRIP, output["w"], TASKBAR_STRIP))


def shoot_panels(desktop, surface, stem):
    """Opens each of the surface's panels, takes it at rest, closes it."""
    for key, namespace, suffix in PANELS[surface]:
        desktop.vela.input("key", key)
        state = desktop.vela.wait_for(lambda s: desktop.layer(s, namespace), timeout=10, what=f"{namespace} open")
        time.sleep(1.2) # its entrance over
        layer = desktop.layer(desktop.vela.state(), namespace) or desktop.layer(state, namespace)
        shoot(desktop, stem.with_name(stem.name + suffix + ".png"), (layer["x"], layer["y"], layer["w"], layer["h"]))
        desktop.vela.input("key", "Escape")
        desktop.vela.wait_for(lambda s: not desktop.layer(s, namespace), timeout=10, what=f"{namespace} closed")
        time.sleep(0.5)


def surface_pictures(desktop, surface):
    """Every style of the surface, then the surface as each theme draws it."""
    directory = PICTURES / surface
    styles = names(THEMES / surface)
    # Pictures of styles that are gone.
    for old in directory.glob("*.png"):
        stem = old.stem.removesuffix("-qs").removesuffix("-nc")
        if stem not in styles:
            old.unlink()
    for style in styles:
        print(f"{surface}: {style}", flush=True)
        use(desktop, surface=surface, style=style)
        if surface == "taskbar":
            shoot_taskbar(desktop, directory / f"{style}.png")
        else:
            shoot_panels(desktop, surface, directory / style)
    for theme in names(THEMES):
        print(f"{surface}: as {theme} draws it", flush=True)
        use(desktop, theme=theme)
        stem = PICTURES / "themes" / f"{theme}-{surface}"
        if surface == "taskbar":
            shoot_taskbar(desktop, stem.with_suffix(".png"))
        else:
            shoot_panels(desktop, surface, stem)


def theme_pictures(desktop):
    """The desktop in each theme, with File Explorer open on the demo folder."""
    desktop.open_app("explorer/vela-files", str(desktop.demo_folder), user="Alex")
    time.sleep(1.5)
    for theme in names(THEMES):
        print(f"themes: {theme}", flush=True)
        use(desktop, theme=theme)
        path = PICTURES / "themes" / f"{theme}.png"
        shoot(desktop, path)
        with Image.open(path) as image:
            small = image.convert("RGB").resize(THEME_SIZE, Image.LANCZOS)
        small.save(path, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", default=str(ROOT / "build"))
    parser.add_argument("--only", choices=(*SURFACES, "themes"))
    args = parser.parse_args()
    build = Path(args.build).resolve()
    core.check_user_namespaces()
    # Only this checkout's themes: gone ones lose their pictures too.
    kept = set()
    for theme in names(THEMES):
        kept |= {theme, f"{theme}-taskbar", f"{theme}-startmenu", f"{theme}-notifications-qs",
                 f"{theme}-notifications-nc"}
    for old in (PICTURES / "themes").glob("*.png"):
        if old.stem not in kept:
            old.unlink()
    jobs = [args.only] if args.only else [*SURFACES, "themes"]
    for job in jobs:
        size = TASKBAR_SIZE if job == "taskbar" else "1920x1080@60"
        with core.Desktop(build, Still(size)) as desktop:
            link_sources(desktop)
            if job == "themes":
                theme_pictures(desktop)
            else:
                surface_pictures(desktop, job)
    return 0


if __name__ == "__main__":
    sys.exit(main())
