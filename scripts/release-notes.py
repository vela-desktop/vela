#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Print the release notes of a Beta from its CHANGELOG.md section.

Settings shows release notes as plain text (docs/updates.md), so Markdown
links become "text (URL)" and relative links point at the repository. The
section is the one `release.py prepare` wrote: "## 0.2.1 — <date>".
Fails when the section is missing or empty, so a tag without notes stops
before anything is built (docs/release-automation.md §7, R4).
"""

import argparse
from pathlib import Path
import re
import sys

REPO = "https://github.com/vela-desktop/vela"
LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)\)")


def section(text, version):
    match = re.search(rf"(?m)^## {re.escape(version)}(?: — [^\n]*)?\n(.*?)(?=^## |\Z)", text, re.S)
    if not match or not match[1].strip():
        raise SystemExit(f"release-notes: no CHANGELOG.md entries for {version}: run scripts/release.py prepare {version}")
    return match[1].strip()


def plain(text, version):
    def link(match):
        label, url = match[1], match[2]
        if not re.match(r"[a-z]+:", url):
            url = f"{REPO}/blob/v{version}/{url.removeprefix('./')}"
        return label if label == url else f"{label} ({url})"
    return LINK.sub(link, text)


def notes(text, version):
    return (f"{plain(section(text, version), version)}\n\n"
            f"Install or update: {REPO}/blob/v{version}/docs/installation.md\n"
            f"Changelog: {REPO}/blob/v{version}/CHANGELOG.md\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("version")
    parser.add_argument("--changelog", default=str(Path(__file__).resolve().parents[1] / "CHANGELOG.md"))
    args = parser.parse_args(argv)
    if not re.fullmatch(r"0\.\d+\.\d+", args.version):
        raise SystemExit(f"release-notes: not a Beta version: {args.version}")
    sys.stdout.write(notes(Path(args.changelog).read_text(encoding="utf-8"), args.version))
    return 0


if __name__ == "__main__":
    sys.exit(main())
