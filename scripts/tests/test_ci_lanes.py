# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("lanes", Path(__file__).parents[1] / "ci-lanes.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def test(name, command, labels=()):
    entry = {"name": name, "command": command, "properties": []}
    if labels:
        entry["properties"].append({"name": "LABELS", "value": list(labels)})
    return entry


class Lanes(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.source = self.root / "src"
        self.build = self.source / "build"
        self.build.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.source}\n")
        self.lanes = m.Build(self.build)
        self.programs = {}
        self.lanes.needs_qt = lambda path, seen=None: self.programs.get(Path(path).name)

    def program(self, name, qt):
        path = self.build / name
        path.write_bytes(b"\x7fELF")
        self.programs[name] = qt
        return str(path)

    def test_a_program_linked_to_qt_is_in_the_qt_lane(self):
        self.assertEqual(self.lanes.test_lane(test("t", [self.program("shell-test", True)])), "qt")

    def test_a_program_without_qt_is_native(self):
        self.assertEqual(self.lanes.test_lane(test("t", [self.program("compositor-test", False)])), "native")

    def test_wrapped_programs_count(self):
        wrapped = test("t", ["/usr/bin/dbus-run-session", "--", self.program("tray-test", True)])
        self.assertEqual(self.lanes.test_lane(wrapped), "qt")
        script = test("t", ["/usr/bin/python3", str(self.source / "fixture.py"), "--",
                            self.program("audio-test", False)])
        self.assertEqual(self.lanes.test_lane(script), "native")

    def test_one_qt_program_is_enough(self):
        both = test("t", [self.program("daemon", False), self.program("client", True)])
        self.assertEqual(self.lanes.test_lane(both), "qt")

    def test_no_build_tree_program_is_a_script(self):
        self.assertEqual(self.lanes.test_lane(test("t", ["/usr/bin/python3", "check.py"])), "scripts")

    def test_programs_outside_the_build_tree_do_not_count(self):
        self.programs["sh"] = False
        self.assertEqual(self.lanes.test_lane(test("t", ["/bin/sh", "-c", "true"])), "scripts")

    def test_gpu_and_rhi_labels_win(self):
        program = self.program("functional", True)
        self.assertEqual(self.lanes.test_lane(test("t", [program], ["functional", "gpu"])), "gpu")
        self.assertEqual(self.lanes.test_lane(test("t", [program], ["rhi"])), "gpu")

    def test_translation_units(self):
        def entry(file, command):
            return {"directory": str(self.build), "file": str(self.source / file), "command": command}

        qt = "clang++ -isystem /usr/include/qt6/QtCore -c"
        (self.build / "compile_commands.json").write_text(json.dumps([
            entry("compositor/src/a.c", "clang -c a.c"),
            entry("compositor/src/a.c", "clang -DTEST -c a.c"),
            entry("shell/src/b.cpp", qt + " b.cpp"),
            entry("compositor/src/color.c", qt + " color.c"),
            entry("compositor/src/color.c", "clang -c color.c"),
            entry("build/shell/vela-shell_autogen/mocs_compilation.cpp", qt + " m.cpp"),
            entry("shell/vela-shell_autogen/moc.cpp", qt + " m.cpp"),
            entry("shell/qml/Panel.qml", qt),
            {"directory": "/", "file": "/usr/src/other.c", "command": "clang -c other.c"},
        ]))
        lanes = self.lanes.source_lanes()
        files = {lane: [Path(e["file"]).relative_to(self.source).as_posix() for e in entries]
                 for lane, entries in lanes.items()}
        self.assertEqual(files["native"], ["compositor/src/a.c", "compositor/src/color.c"])
        self.assertEqual(files["qt"], ["shell/src/b.cpp", "compositor/src/color.c"])


class Needed(unittest.TestCase):
    def test_reads_a_real_programs_libraries(self):
        names = m.needed(Path(os.path.realpath(sys.executable)))
        self.assertIsNotNone(names)
        self.assertTrue(any(name.startswith("libc.so") for name in names), names)

    def test_a_script_is_not_elf(self):
        self.assertIsNone(m.needed(Path(__file__)))

    def test_a_missing_file_is_not_elf(self):
        self.assertIsNone(m.needed(Path("/nonexistent/vela")))


if __name__ == "__main__":
    unittest.main()
