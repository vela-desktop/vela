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
    re.compile(r"\bQProcess::(?:startDetached|execute)\s*\("),
    re.compile(r"\bQDesktopServices::openUrl\s*\("),
    re.compile(r"\bQt\.openUrlExternally\s*\("),
    # Raw instances bypass the boundary through both start() and startDetached().
    re.compile(r"\bQProcess\s+[A-Za-z_]\w*\s*(?=[;({=])"),
    re.compile(r"\bQProcess\s*[({]"),
    re.compile(r"\bnew\s+QProcess\b"),
    re.compile(r"\bmake_(?:unique|shared)\s*<\s*QProcess\s*>"),
)


def violations(code):
    """Find direct launch APIs and raw process construction in production UI code."""
    return [(code.count("\n", 0, match.start()) + 1, match.group())
            for pattern in FORBIDDEN for match in pattern.finditer(code)]


class ExternalProcessPolicy(unittest.TestCase):
    def test_ui_launches_use_sanitized_environment(self):
        """Require UI production sources to use ExternalProcess or the shared launcher."""
        found = []
        for scope in SCOPES:
            for path in (ROOT / scope).rglob("*"):
                # Fixtures intentionally run raw processes and private D-Bus daemons.
                if path.suffix not in (".cpp", ".h", ".qml") or "tests" in path.relative_to(ROOT).parts:
                    continue
                code = path.read_text(encoding="utf-8")
                for line, match in violations(code):
                    found.append(f"{path.relative_to(ROOT)}:{line}: {match}")
        self.assertFalse(
            found,
            "Use vela::process::ExternalProcess/startDetached or System.openUrl:\n"
            + "\n".join(found),
        )

    def test_rejects_raw_instance_launches(self):
        """Stack, heap and smart-pointer instances must not bypass either launch form."""
        for code in (
            'QProcess child; child.start("app");',
            'QProcess child(parent); child.startDetached();',
            'QProcess child{parent}; child.start();',
            'auto child = QProcess{}; child.startDetached();',
            'auto child = QProcess(parent); child.start();',
            'auto* child = new QProcess(parent); child->start("app");',
            'auto child = std::make_unique<QProcess>(); child->startDetached();',
            'auto child = std::make_shared<QProcess>(); child->start();',
        ):
            with self.subTest(code=code):
                self.assertTrue(violations(code))

    def test_allows_the_filtered_wrapper(self):
        """Wrapper instances and QProcess signal or enum references remain valid."""
        code = '''
            vela::process::ExternalProcess child;
            child.start("app");
            auto* detached = new vela::process::ExternalProcess(parent);
            detached->startDetached();
            connect(&child, &QProcess::finished, receiver, slot);
            const auto state = QProcess::NotRunning;
        '''
        self.assertFalse(violations(code))

    def test_folder_opener_is_a_runtime_dependency(self):
        """Every supported package must install the helper used to open theme folders."""
        for name in ("PKGBUILD", "PKGBUILD.beta"):
            contents = (ROOT / "packaging/arch" / name).read_text()
            self.assertRegex(contents, r"(?s)depends=\(.*?'xdg-utils'.*?\)")
        fedora = (ROOT / "packaging/fedora/vela.spec").read_text()
        self.assertRegex(fedora, r"(?m)^Requires:\s+xdg-utils$")
        cmake = (ROOT / "CMakeLists.txt").read_text()
        dependencies = re.search(r'set\(CPACK_DEBIAN_PACKAGE_DEPENDS\s+"([^"]+)"\)', cmake)
        self.assertIsNotNone(dependencies)
        self.assertIn("xdg-utils", [name.strip() for name in dependencies.group(1).split(",")])


if __name__ == "__main__":
    unittest.main()
