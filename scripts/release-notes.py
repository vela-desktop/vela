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
CHANGELOG = Path(__file__).resolve().parents[1] / "CHANGELOG.md"
LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)\)")


def section(text, version):
    match = re.search(rf"(?m)^## {re.escape(version)}(?: — [^\n]*)?\n(.*?)(?=^## |\Z)", text, re.S)
    if not match or not match[1].strip():
        raise SystemExit(f"release-notes: no CHANGELOG.md entries for {version}: run scripts/release.py prepare {version}")
    return match[1].strip()


def plain(text, source_ref):
    def link(match):
        label, url = match[1], match[2]
        if url.startswith("#"):
            url = f"{REPO}/blob/{source_ref}/CHANGELOG.md{url}"
        elif not re.match(r"[a-z]+:", url):
            url = f"{REPO}/blob/{source_ref}/{url.removeprefix('./')}"
        return label if label == url else f"{label} ({url})"
    return LINK.sub(link, text)


def notes(text, version, source_ref=None):
    source_ref = source_ref or f"v{version}"
    return (f"{plain(section(text, version), source_ref)}\n\n"
            f"Install or update: {REPO}/blob/{source_ref}/docs/installation.md\n"
            f"Changelog: {REPO}/blob/{source_ref}/CHANGELOG.md\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("version")
    parser.add_argument("--source-ref", help="Git ref used in changelog links (default: vVERSION)")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"0\.\d+\.\d+", args.version):
        raise SystemExit(f"release-notes: not a Beta version: {args.version}")
    sys.stdout.write(notes(CHANGELOG.read_text(encoding="utf-8"), args.version, args.source_ref))
    return 0


if __name__ == "__main__":
    sys.exit(main())
