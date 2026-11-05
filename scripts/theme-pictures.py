#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The pictures Settings > Personalization shows (themes/screenshots/): every
theme and every surface style, as the real shell draws it, on a headless Vela
with neutral data (recording_core.Desktop: a made-up user, demo apps, private
D-Bus and PipeWire, English text) and a stopped clock (VELA_STILL), so that
two runs give the same pictures whatever the day. Not drawn by hand, and
never an author's screenshot (docs/theme-system.md, "Preview and user
interaction").

  themes/screenshots/themes/<theme>.png             the desktop in the theme
  themes/screenshots/themes/<theme>/<surface>.png   a surface as it draws it
  themes/screenshots/<surface>/<style>.png          a style
  (-qs.png and -nc.png for "notifications": quick settings and the
  notification center)

Usage: scripts/theme-pictures.py [--build build] [--only taskbar|startmenu|notifications|themes]
Run it again after changing a theme, a style or what they draw: it rewrites
every picture and removes those of themes and styles that no longer exist.
"""

import argparse
import os
from pathlib import Path
import shutil
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
# A theme's picture: the desktop with Settings open on Colors at its top
# left and quick settings above the tray, a third of 1920 x 1080. What tells
# themes apart shows at a card's size: the window's glass over the
# wallpaper, the taskbar's shape, where quick settings open and how.
THEME_SIZE = (640, 360)
SETTINGS_AT = (90, 60)
# Start and the notification center are taller than Settings shows them (220
# units, twice that at 200%): their pictures are no taller than this.
PANEL_HEIGHT = 480
# What the clock and the calendar show, whatever the day of the run.
STILL = "2026-01-14T10:30:00"


class Still(core.Scene):
    name = "theme-pictures"
    env = {"VELA_STILL": STILL}

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


def shoot(desktop, path, region=None, height=None):
    """A picture of the output, or of a region of it, no taller than height."""
    path.parent.mkdir(parents=True, exist_ok=True)
    desktop.vela.screenshot(str(path), region=region)
    with Image.open(path) as image:
        image.load()
    if height and image.height > height:
        image = image.resize((round(image.width * height / image.height), height), Image.LANCZOS)
    image.save(path, optimize=True)


def shoot_taskbar(desktop, path):
    output = desktop.vela.state()["outputs"][0]
    shoot(desktop, path, (0, output["h"] - TASKBAR_STRIP, output["w"], TASKBAR_STRIP))


def open_panel(desktop, key, namespace):
    desktop.vela.input("key", key)
    state = desktop.vela.wait_for(lambda s: desktop.layer(s, namespace), timeout=10, what=f"{namespace} open")
    time.sleep(1.2) # its entrance over
    return desktop.layer(desktop.vela.state(), namespace) or desktop.layer(state, namespace)


def close_panel(desktop, namespace):
    desktop.vela.input("key", "Escape")
    desktop.vela.wait_for(lambda s: not desktop.layer(s, namespace), timeout=10, what=f"{namespace} closed")
    time.sleep(0.5)


def shoot_panels(desktop, surface, stem):
    """Opens each of the surface's panels, takes it at rest, closes it."""
    for key, namespace, suffix in PANELS[surface]:
        layer = open_panel(desktop, key, namespace)
        shoot(desktop, stem.with_name(stem.name + suffix + ".png"),
              (layer["x"], layer["y"], layer["w"], layer["h"]), height=PANEL_HEIGHT)
        close_panel(desktop, namespace)


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
        stem = PICTURES / "themes" / theme / surface
        if surface == "taskbar":
            shoot_taskbar(desktop, stem.with_suffix(".png"))
        else:
            shoot_panels(desktop, surface, stem)


def theme_pictures(desktop):
    """The desktop in each theme: Settings open on Colors (Window Glass's
    is see-through), quick settings open above the tray, the taskbar."""
    settings = desktop.open_app("settings/vela-settings", "--page", "colors", user="Alex")
    time.sleep(1.5)
    # Dragged by its title bar (the compositor's) to the top left: the rest
    # of the desktop stays free for the wallpaper, quick settings, the bar.
    window = desktop.vela.state().window(settings)
    x, y = window["x"] + window["w"] // 2, window["y"] + 16
    dx, dy = SETTINGS_AT[0] - window["x"], SETTINGS_AT[1] - window["y"]
    desktop.vela.input("move", x, y, "sleep", "300", "down", "sleep", "100", "move", x + 8, y + 4, "sleep", "50",
                       "move", x + dx // 2, y + dy // 2, "sleep", "50", "move", x + dx, y + dy, "sleep", "100",
                       "up", "sleep", "500")
    for theme in names(THEMES):
        print(f"themes: {theme}", flush=True)
        use(desktop, theme=theme)
        open_panel(desktop, "super+a", "vela-quick-settings")
        path = PICTURES / "themes" / f"{theme}.png"
        shoot(desktop, path)
        close_panel(desktop, "vela-quick-settings")
        with Image.open(path) as image:
            small = image.convert("RGB").resize(THEME_SIZE, Image.LANCZOS)
        small.save(path, optimize=True)


def remove_gone_themes():
    """Only this checkout's themes keep pictures: a desktop picture and a
    folder each."""
    themes = set(names(THEMES))
    directory = PICTURES / "themes"
    directory.mkdir(parents=True, exist_ok=True)
    for old in directory.iterdir():
        if old.is_dir() and old.name not in themes:
            shutil.rmtree(old)
        elif old.is_file() and (old.suffix != ".png" or old.stem not in themes):
            old.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", default=str(ROOT / "build"))
    parser.add_argument("--only", choices=(*SURFACES, "themes"))
    args = parser.parse_args()
    # A build of this checkout: the pictures are this checkout's, and a path
    # given here leads nowhere else.
    build = os.path.realpath(args.build)
    if os.path.commonpath([build, str(ROOT)]) != str(ROOT):
        parser.error(f"--build: not a directory of this checkout ({ROOT})")
    if not os.path.isfile(os.path.join(build, "compositor", "vela-compositor")):
        parser.error(f"--build: {build} has no compositor/vela-compositor; build Vela first")
    core.check_user_namespaces()
    remove_gone_themes()
    jobs = [args.only] if args.only else [*SURFACES, "themes"]
    for job in jobs:
        size = TASKBAR_SIZE if job == "taskbar" else "1920x1080@60"
        with core.Desktop(Path(build), Still(size)) as desktop:
            link_sources(desktop)
            if job == "themes":
                theme_pictures(desktop)
            else:
                surface_pictures(desktop, job)
    return 0


if __name__ == "__main__":
    sys.exit(main())
