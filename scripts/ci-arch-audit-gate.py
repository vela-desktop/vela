#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Block the NVIDIA CI image on fixable high or critical Arch advisories.

Trivy has no Arch Linux driver, so it cannot see pacman packages; the
publication workflow runs `arch-audit --upgradable --json` inside the image
instead and pipes the saved output to this script's standard input. `--upgradable` keeps only the advisory
groups whose fix is already in the repos, i.e. the fixable ones. Output that
is not the shape arch-audit prints fails too: an assessment that could not
be read must not count as a clean one.

Exit status: 0 nothing blocking, 1 blocking advisories, 2 unreadable report.
"""

import json
import sys

BLOCKING = {"High", "Critical"}
FIELDS = ("name", "packages", "severity", "fixed", "issues")


def blocking_groups(groups):
    """Return the groups that block publication; ValueError on bad input."""
    if not isinstance(groups, list):
        raise ValueError("expected a JSON list of advisory groups")
    found = []
    for group in groups:
        if not isinstance(group, dict) or any(f not in group for f in FIELDS):
            raise ValueError(f"malformed advisory group: {group!r}")
        if not isinstance(group["packages"], list) or not group["packages"]:
            raise ValueError(f"advisory group without packages: {group['name']}")
        if group["severity"] in BLOCKING:
            found.append(group)
    return found


def describe(group):
    issues = ", ".join(group["issues"]) or "no CVE"
    return (f"{group['name']} ({group['severity']}) in "
            f"{', '.join(group['packages'])}: fixed in {group['fixed']} [{issues}]")


def main(report):
    try:
        found = blocking_groups(json.load(report))
    except ValueError as error:
        print(f"::error::cannot assess pacman packages: {error}")
        return 2
    for group in found:
        print(f"::error::fixable advisory {describe(group)}")
    if found:
        return 1
    print("No fixable high or critical Arch advisories.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.stdin))
