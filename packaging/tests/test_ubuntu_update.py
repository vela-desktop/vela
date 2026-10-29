#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline Ubuntu 26.10 updater contracts: real Git, mocked APT/CMake/CPack."""

import json
from pathlib import Path

from test_update import UpdateFixture


UBUNTU_STUB = r'''#!/usr/bin/env python3
import json, os, pathlib, sys

name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
state_file = pathlib.Path(os.environ["TEST_DEB_STATE"])
calls_file = pathlib.Path(os.environ["TEST_CALLS"])

def record(**values):
    with calls_file.open("a") as log:
        log.write(json.dumps(dict(tool=name, args=args, **values)) + "\n")

if name == "dpkg-query":
    if not state_file.exists():
        sys.exit(1)
    state = json.loads(state_file.read_text())
    print(state["status"] + "\t" + state["version"])
elif name == "dpkg-deb":
    data = json.loads(pathlib.Path(args[1]).read_text())
    print(data[args[2]])
elif name == "cmake":
    record()
    if args[:1] == ["--build"]:
        if os.environ.get("TEST_FAIL_BUILD"):
            sys.exit(1)
    else:
        source = pathlib.Path(args[args.index("-S") + 1])
        build = pathlib.Path(args[args.index("-B") + 1])
        assert (source / "packaging/vela-update").is_file()
        sha = next(a.split("=", 1)[1] for a in args
                   if a.startswith("-DVELA_PACKAGE_COMMIT="))
        build.mkdir(parents=True, exist_ok=True)
        (build / "selected-sha").write_text(sha)
elif name == "cpack":
    record()
    if os.environ.get("TEST_FAIL_CPACK"):
        sys.exit(1)
    build = pathlib.Path(args[args.index("--config") + 1]).parent
    dest = pathlib.Path(args[args.index("-B") + 1])
    sha = (build / "selected-sha").read_text()
    dest.mkdir(parents=True, exist_ok=True)
    metadata = {
        "Package": "wrong-package" if os.environ.get("TEST_WRONG_DEB_PACKAGE") else "vela-git",
        "Version": "0.1.0" if os.environ.get("TEST_BAD_DEB_VERSION")
                   else "0.1.0+git" + sha,
    }
    (dest / ("vela-git_0.1.0+git" + sha + "_amd64.deb")).write_text(json.dumps(metadata))
elif name == "sudo":
    record()
    if args[:2] == ["apt-get", "update"] and os.environ.get("TEST_FAIL_APT_UPDATE"):
        sys.exit(1)
    if args[:2] == ["apt-get", "install"] and "--reinstall" in args:
        if os.environ.get("TEST_FAIL_INSTALL"):
            sys.exit(1)
        package = json.loads(pathlib.Path(args[-1]).read_text())
        state_file.write_text(json.dumps(
            {"status": "install ok installed", "version": package["Version"]}))
elif name == "apt-get":
    record()
else:
    sys.exit("Unexpected Ubuntu stub: " + name)
'''


class UbuntuFixture(UpdateFixture):
    def setUp(self):
        super().setUp()
        self.os_release.write_text('ID=ubuntu\nVERSION_ID="26.10"\n')
        bin_dir = self.directory / "bin"
        (bin_dir / "pacman").unlink()  # Other Debian derivatives must not select Arch.
        for name in ("dpkg-query", "dpkg-deb", "apt-get", "cmake", "cpack", "sudo"):
            stub = bin_dir / name
            stub.write_text(UBUNTU_STUB)
            stub.chmod(0o755)
        self.state_file = self.directory / "dpkg-state.json"
        self.env["TEST_DEB_STATE"] = str(self.state_file)

    def set_installed(self, sha, status="install ok installed"):
        self.state_file.write_text(json.dumps({
            "status": status, "version": "0.1.0+git" + sha[:7],
        }))

    def installed_version(self):
        return json.loads(self.state_file.read_text())["version"]

    def package_install_calls(self):
        return [c for c in self.calls("sudo")
                if c["args"][:2] == ["apt-get", "install"]
                and "--reinstall" in c["args"]]


class UbuntuUpdates(UbuntuFixture):
    def test_bootstrap_builds_selected_commit_and_installs_local_deb(self):
        self.run_update("--build", "--jobs", "1")
        configure = [c for c in self.calls("cmake")
                     if c["args"][:1] != ["--build"]]
        self.assertEqual(len(configure), 1)
        self.assertIn("-DVELA_PACKAGE_COMMIT=" + self.main[:7], configure[0]["args"])
        build = next(c for c in self.calls("cmake") if c["args"][:1] == ["--build"])
        self.assertEqual(build["args"][-1], "1")
        self.assertEqual(self.installed_version(), "0.1.0+git" + self.main[:7])
        installs = self.package_install_calls()
        self.assertEqual(len(installs), 1)
        self.assertIn("--allow-downgrades", installs[0]["args"])
        self.assertEqual(len(list((self.mirror.parent / "packages").glob("*.deb"))), 1)
        self.run_update("--check", status=1)
        self.assertEqual(len(self.package_install_calls()), 1)

    def test_check_reports_pending_changes_without_building(self):
        self.set_installed(self.base)
        result = self.run_update("--check")
        self.assertIn("What's new", result.stdout)
        self.assertEqual(self.calls("cmake"), [])
        self.assertEqual(self.calls("sudo"), [])

    def test_check_identical_commit_is_noop(self):
        self.set_installed(self.main)
        self.run_update("--check", status=1)
        self.run_update()
        self.assertEqual(self.package_install_calls(), [])
        self.assertEqual(self.calls("cmake"), [])

    def test_force_reinstalls_same_version(self):
        self.set_installed(self.main)
        self.run_update("--force", "--build")
        self.assertEqual(self.installed_version(), "0.1.0+git" + self.main[:7])
        self.assertEqual(len(self.package_install_calls()), 1)

    def test_commit_downgrade_installs_older_revision(self):
        self.set_installed(self.main)
        self.run_update("--commit", self.base, "--build")
        self.assertEqual(self.installed_version(), "0.1.0+git" + self.base[:7])
        self.assertIn("--allow-downgrades", self.package_install_calls()[0]["args"])

    def test_download_does_not_consume_unsigned_ci_artifacts(self):
        self.env.update(TEST_CI_RUN="123", TEST_RELEASE_SHA=self.main,
                        TEST_DOWNLOAD_SHA=self.main[:7])
        self.run_update("--download")
        self.assertEqual(len(self.package_install_calls()), 1)
        self.assertEqual(self.calls("curl"), [])
        self.assertEqual(self.calls("gh"), [])
        self.assertEqual(self.calls("cosign"), [])

    def test_unconfigured_dpkg_entry_is_not_considered_installed(self):
        self.set_installed(self.main, status="install ok unpacked")
        self.run_update("--check", status=0)
        self.run_update("--build")
        self.assertEqual(json.loads(self.state_file.read_text())["status"],
                         "install ok installed")
        self.assertEqual(len(self.package_install_calls()), 1)

    def test_failed_build_does_not_replace_installed_desktop(self):
        self.set_installed(self.base)
        self.env["TEST_FAIL_BUILD"] = "1"
        self.run_update("--build", status=1)
        self.assertEqual(self.installed_version(), "0.1.0+git" + self.base[:7])
        self.assertEqual(self.package_install_calls(), [])

    def test_failed_apt_install_keeps_cached_deb(self):
        self.set_installed(self.base)
        self.env["TEST_FAIL_INSTALL"] = "1"
        result = self.run_update("--build", status=1)
        self.assertIn("package is ready but wasn't installed", result.stderr)
        self.assertEqual(self.installed_version(), "0.1.0+git" + self.base[:7])
        self.assertEqual(len(self.package_install_calls()), 1)
        self.assertEqual(len(list((self.mirror.parent / "packages").glob("*.deb"))), 1)

    def test_rejects_deb_without_selected_git_revision(self):
        self.set_installed(self.base)
        self.env["TEST_BAD_DEB_VERSION"] = "1"
        result = self.run_update("--build", status=1)
        self.assertIn("does not identify selected commit", result.stderr)
        self.assertEqual(self.package_install_calls(), [])
        self.assertEqual(self.installed_version(), "0.1.0+git" + self.base[:7])

    def test_rejects_package_with_unexpected_name(self):
        self.env["TEST_WRONG_DEB_PACKAGE"] = "1"
        result = self.run_update("--build", status=1)
        self.assertIn("valid vela-git DEB", result.stderr)
        self.assertEqual(self.package_install_calls(), [])

    def test_rejects_other_ubuntu_releases(self):
        self.os_release.write_text('ID=ubuntu\nVERSION_ID="24.04"\n')
        result = self.run_update("--check", status=1)
        self.assertIn("Ubuntu 26.10", result.stderr)
        self.assertEqual(self.calls("sudo"), [])

    def test_rejects_other_debian_derivatives(self):
        self.os_release.write_text('ID=debian\nID_LIKE=ubuntu\nVERSION_ID="26.10"\n')
        result = self.run_update("--check", status=1)
        self.assertIn("unsupported distribution", result.stderr)
        self.assertEqual(self.calls("sudo"), [])

    def test_dependency_failure_does_not_build_or_install(self):
        self.env["TEST_FAIL_APT_UPDATE"] = "1"
        self.run_update("--build", status=1)
        self.assertEqual(self.calls("cmake"), [])
        self.assertEqual(self.package_install_calls(), [])
