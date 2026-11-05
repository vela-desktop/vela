#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""UI applications must use the Vela process-launch environment boundary."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCOPES = ("shell", "settings", "explorer", "polkit/prompt")
FORBIDDEN = (
    re.compile(r"\bQProcess::startDetached\s*\("),
    re.compile(r"\bQDesktopServices::openUrl\s*\("),
    re.compile(r"\bQt\.openUrlExternally\s*\("),
)


class ExternalProcessPolicy(unittest.TestCase):
    def test_ui_launches_use_sanitized_environment(self):
        violations = []
        for scope in SCOPES:
            for path in (ROOT / scope).rglob("*"):
                if path.suffix not in (".cpp", ".h", ".qml"):
                    continue
                code = path.read_text(encoding="utf-8")
                for pattern in FORBIDDEN:
                    for match in pattern.finditer(code):
                        line = code.count("\n", 0, match.start()) + 1
                        violations.append(f"{path.relative_to(ROOT)}:{line}: {match.group()}")
        self.assertFalse(
            violations,
            "Use vela::process::ExternalProcess/startDetached or System.openUrl:\n"
            + "\n".join(violations),
        )


if __name__ == "__main__":
    unittest.main()
