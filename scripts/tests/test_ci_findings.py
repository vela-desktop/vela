# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import importlib.util
import io
import json
import shutil
import sys
import tempfile
import time
from pathlib import Path
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("findings", Path(__file__).parents[1] / "ci-findings.py")
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m  # dataclasses look their module up
spec.loader.exec_module(m)

SOURCE = Path("/src/vela")


class Parse(unittest.TestCase):
    def test_clang_tidy(self):
        text = (f"{SOURCE}/compositor/src/a.c:12:5: warning: Null pointer passed [clang-analyzer-core.NonNullParamChecker]\n"
                f"{SOURCE}/compositor/src/a.c:10:1: note: Assuming the condition is true\n"
                f"{SOURCE}/compositor/src/a.c:12:5: warning: Null pointer passed [clang-analyzer-core.NonNullParamChecker]\n")
        found = m.parse("clang-tidy", text, SOURCE)
        self.assertEqual(found, {m.Finding("clang-tidy", "clang-analyzer-core.NonNullParamChecker",
                                           "compositor/src/a.c", 12, "Null pointer passed")})

    def test_one_finding_per_listed_check(self):
        text = f"{SOURCE}/a.cpp:1:1: warning: twice [bugprone-x,cert-y]\n"
        self.assertEqual({f.check for f in m.parse("clang-tidy", text, SOURCE)}, {"bugprone-x", "cert-y"})

    def test_brackets_inside_the_message(self):
        text = (f"{SOURCE}/a.cpp:1:1: warning: index [i] of list [x] [bugprone-y]\n"
                f"{SOURCE}/a.cpp:2:1: warning: no check at the end [bugprone-y] here\n"
                f"{SOURCE}/a.cpp:3:1: warning: empty check []\n")
        (finding,) = m.parse("clang-tidy", text, SOURCE)
        self.assertEqual((finding.line, finding.message, finding.check), (1, "index [i] of list [x]", "bugprone-y"))

    def test_long_lines_parse_in_linear_time(self):
        # A message full of " [" made the old pattern backtrack quadratically.
        line = f"{SOURCE}/a.cpp:1:1: warning: " + " [" * 200_000 + "x\n"
        start = time.monotonic()
        self.assertEqual(m.parse("clang-tidy", line, SOURCE), set())
        self.assertLess(time.monotonic() - start, 1.0)

    def test_clazy_checks_only(self):
        text = (f"{SOURCE}/shell/src/a.cpp:3:9: warning: Mixing iterators [-Wclazy-strict-iterators]\n"
                f"{SOURCE}/shell/src/a.cpp:4:9: warning: add explicit braces [-Wdangling-else]\n")
        found = m.parse("clazy", text, SOURCE)
        self.assertEqual([f.check for f in found], ["strict-iterators"])

    def test_cppcheck(self):
        text = f"{SOURCE}/brightness/src/ddc.c:40: error: Uninitialized variable: x [uninitvar]\n"
        (finding,) = m.parse("cppcheck", text, SOURCE)
        self.assertEqual((finding.check, finding.file, finding.line), ("uninitvar", "brightness/src/ddc.c", 40))

    def test_qmllint(self):
        text = (f"Warning: {SOURCE}/shell/qml/Panel.qml:7:3: Unqualified access [unqualified]\n"
                "    width: parent.width\n"
                f"Info: {SOURCE}/shell/qml/Panel.qml:8:1: Something [unqualified]\n")
        (finding,) = m.parse("qmllint", text, SOURCE)
        self.assertEqual((finding.check, finding.file, finding.line), ("unqualified", "shell/qml/Panel.qml", 7))

    def test_only_first_party_files(self):
        text = (f"/usr/include/qt6/QtCore/qlist.h:9:1: warning: x [clang-analyzer-core.A]\n"
                f"{SOURCE}/build-analysis/shell/proto.h:9:1: warning: x [clang-analyzer-core.A]\n"
                f"{SOURCE}/build/shell/vela-shell_autogen/moc.cpp:9:1: warning: x [clang-analyzer-core.A]\n"
                f"{SOURCE}/shell/src/x_autogen/moc.cpp:9:1: warning: x [clang-analyzer-core.A]\n")
        self.assertEqual(m.parse("clang-tidy", text, SOURCE), set())

    def test_a_cppcheck_failure_counts_even_in_generated_files(self):
        text = (f"{SOURCE}/build/common/x_autogen/include/x.moc:18: error: #error [preprocessorErrorDirective]\n"
                f"{SOURCE}/build/capture/protocols/p.h:186: warning: cast [dangerousTypeCast]\n")
        found = m.parse("cppcheck", text, SOURCE)
        self.assertEqual([f.check for f in found], ["preprocessorErrorDirective"])


class Ceilings(unittest.TestCase):
    def findings(self, *checks):
        return {"clazy": {m.Finding("clazy", check, "a.cpp", line, "x") for line, check in enumerate(checks)}}

    def test_at_the_ceiling_passes(self):
        failures, rows = m.compare("qt", self.findings("a", "a"), {"qt": {"clazy": {"a": 2}}})
        self.assertEqual(failures, [])
        self.assertIn("ok", rows[0])

    def test_above_the_ceiling_lists_the_findings(self):
        failures, _ = m.compare("qt", self.findings("a", "a", "b"), {"qt": {"clazy": {"a": 2}}})
        self.assertEqual(len(failures), 1)
        self.assertIn("qt clazy b: 1 findings, ceiling 0", failures[0])
        self.assertIn("a.cpp:2: x [b]", failures[0])

    def test_below_the_ceiling_asks_to_lower_it(self):
        failures, _ = m.compare("qt", self.findings("a"), {"qt": {"clazy": {"a": 2}}})
        self.assertEqual(len(failures), 1)
        self.assertIn("below the ceiling of 2", failures[0])

    def test_a_check_gone_entirely_also_asks(self):
        failures, _ = m.compare("qt", self.findings(), {"qt": {"clazy": {"a": 1}}})
        self.assertEqual(len(failures), 1)

    def test_ceilings_of_another_lane_or_tool_do_not_apply(self):
        failures, _ = m.compare("native", self.findings("a"), {"qt": {"clazy": {"a": 1}}})
        self.assertEqual(len(failures), 1)
        failures, _ = m.compare("qt", {"cppcheck": set()}, {"qt": {"clazy": {"a": 1}}})
        self.assertEqual(failures, [])

    def test_update_writes_the_counts_and_drops_empty_tools(self):
        ceilings = {"qt": {"clazy": {"a": 5}, "qmllint": {"import": 3}}}
        updated = m.update("qt", {"clazy": set(), "qmllint": {m.Finding("qmllint", "unqualified", "a.qml", 1, "x")}},
                           ceilings)
        self.assertEqual(updated, {"qt": {"qmllint": {"unqualified": 1}}})


class Main(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.log = self.root / "clazy.log"
        self.log.write_text(f"{self.root}/a.cpp:1:1: warning: x [-Wclazy-strict-iterators]\n")
        self.ceilings = self.root / "ceilings.json"
        patcher = mock.patch.object(m, "CEILINGS", self.ceilings)
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_main(self, *extra):
        out = io.StringIO()
        with mock.patch.dict("os.environ", {"GITHUB_STEP_SUMMARY": ""}), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            status = m.main(["qt", "--clazy", str(self.log), "--source", str(self.root), *extra])
        return status, out.getvalue()

    def test_a_missing_ceilings_file_means_zero(self):
        status, output = self.run_main()
        self.assertEqual(status, 1)
        self.assertIn("ceiling 0", output)

    def test_update_then_pass(self):
        self.assertEqual(self.run_main("--update")[0], 0)
        self.assertEqual(json.loads(self.ceilings.read_text()), {"qt": {"clazy": {"strict-iterators": 1}}})
        self.assertEqual(self.run_main()[0], 0)

    def test_list_goes_to_standard_output(self):
        self.ceilings.write_text('{"qt": {"clazy": {"strict-iterators": 1}}}')
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.dict("os.environ", {"GITHUB_STEP_SUMMARY": ""}), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = m.main(["qt", "--clazy", str(self.log), "--source", str(self.root), "--list"])
        self.assertEqual(status, 0)
        self.assertEqual(out.getvalue(), "clazy: a.cpp:1: x [strict-iterators]\n")
        self.assertIn("Static analysis, qt lane", err.getvalue())

    def test_the_repository_ceilings_are_valid(self):
        ceilings = json.loads((m.ROOT / m.CEILINGS_NAME).read_text(encoding="utf-8"))
        for lane, tools in ceilings.items():
            self.assertIn(lane, m.LANES)
            for tool, counts in tools.items():
                self.assertIn(tool, m.TOOLS)
                self.assertTrue(all(isinstance(n, int) and n > 0 for n in counts.values()), counts)


if __name__ == "__main__":
    unittest.main()
