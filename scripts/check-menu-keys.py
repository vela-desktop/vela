#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""The keys of the context menu entries the user can hide (docs/renderer.md
§14.12): those the menus give their entries (the shell's and Files' QML) and
those Settings > Personalization > Context menu lists must be the same, or a
switch would hide nothing, or an entry would have no switch."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MENUS = [ROOT / "shell/qml", ROOT / "explorer/qml"]
PAGE = ROOT / "settings/qml/pages/ContextMenuPage.qml"
GROUPS = ("desktop", "file", "folder", "taskbar", "start", "winx")
KEY = rf"(?:{'|'.join(GROUPS)})\.\w+"


def strip_comments(text: str) -> str:
    """The QML without its // and /* */ comments, strings left whole: a
    commented-out entry counts on neither side."""
    out = []
    i, quote = 0, None
    while i < len(text):
        c = text[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'`":
            quote = c
            out.append(c)
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = len(text) if end < 0 else end
            continue
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = len(text) if end < 0 else end + 2
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def menu_keys(text: str) -> set[str]:
    """`key: "file.copy"`, `key: pinned ? "taskbar.pinned" : ...` and the
    helpers taking the key first (`item("winx.files", ...)`)."""
    keys = set()
    text = strip_comments(text)
    for line in re.findall(r"key:[^,}]*", text):
        keys.update(re.findall(rf'"({KEY})"', line))
    keys.update(re.findall(rf'\w+\("({KEY})",', text))
    return keys


def page_keys(text: str) -> set[str]:
    keys = set()
    text = strip_comments(text)
    for group in re.findall(r"keys:\s*\[([^\]]*)\]", text):
        keys.update(re.findall(rf'"({KEY})"', group))
    return keys


def check(menus: str, page: str) -> list[str]:
    used, listed = menu_keys(menus), page_keys(page)
    return ([f"{k}: in a menu, not in Settings" for k in sorted(used - listed)]
            + [f"{k}: in Settings, in no menu" for k in sorted(listed - used)])


def main() -> int:
    menus = "\n".join(p.read_text(encoding="utf-8") for d in MENUS for p in sorted(d.glob("*.qml")))
    failures = check(menus, PAGE.read_text(encoding="utf-8"))
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
