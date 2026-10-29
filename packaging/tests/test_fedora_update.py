#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline Fedora updater contracts using real Git and simulated dnf/rpmbuild."""

from pathlib import Path

from test_update import ROOT, STUB, UpdateFixture


class FedoraFixture(UpdateFixture):
    def setUp(self):
        super().setUp()
        tools = self.directory / "bin"
        (tools / "pacman").unlink()  # Detect rpm/dnf on the non-Fedora CI host.
        for name in ("rpm", "dnf", "rpmbuild"):
            stub = tools / name
            stub.write_text(STUB)
            stub.chmod(0o755)
        self.env["TEST_FEDORA"] = "1"
        # Base fixture already retargets the disposable updater copy.
        self.os_release.write_text("ID=fedora\n")
        # The source archive version is read from the selected Git commit.
        (self.source / "CMakeLists.txt").write_text("project(vela\n    VERSION 0.1.0\n)\n")
        fedora = self.source / "packaging/fedora"
        fedora.mkdir(parents=True)
        self.spec = fedora / "vela.spec"
        self.spec.write_text((ROOT / "packaging/fedora/vela.spec").read_text())
        self.main = self.commit("add Fedora spec")
        self.git("checkout", "-B", "fix/nvidia", self.main)
        self.spec.write_text(self.spec.read_text() + "\n# branch spec\n")
        self.nvidia = self.commit("test Fedora branch")
        self.git("checkout", "main")


class FedoraUpdates(FedoraFixture):
    def test_rpm_declares_updater_dependencies(self):
        # The RPM must work when installed directly, not only through the
        # README bootstrap command that already installs these utilities.
        spec = self.spec.read_text()
        required = {
            line.split(":", 1)[1].strip()
            for line in spec.splitlines() if line.startswith("Requires:")
        }
        recommended = {
            line.split(":", 1)[1].strip()
            for line in spec.splitlines() if line.startswith("Recommends:")
        }
        for name in ("git", "curl", "util-linux", "rpm", "dnf5"):
            self.assertIn(name, required)
        for name in ("rpm-build", "dnf5-plugins"):
            self.assertIn(name, recommended)

    def test_multiple_runs_select_the_fedora_artifact(self):
        self.env.update(TEST_CI_RUNS="303,202,101",
                        TEST_CI_MISSING_RUNS="303,202",
                        TEST_DOWNLOAD_SHA=self.main[:7])
        self.run_update("--download")
        download = next(c for c in self.calls("gh")
                        if c["args"][:2] == ["run", "download"])
        self.assertEqual(download["args"][2], "101")
        self.assertEqual(
            download["args"][download["args"].index("-n") + 1],
            "vela-fedora-rpm",
        )
        self.assertEqual(self.calls("rpmbuild"), [])
        self.assertEqual(self.calls("sudo")[0]["args"][:3], ["dnf", "install", "-y"])

    def test_check_is_read_only(self):
        self.run_update("--check")
        self.assertEqual(self.calls("rpmbuild"), [])
        self.assertEqual(self.calls("sudo"), [])

    def test_installed_commit_is_identified_with_rpm(self):
        self.env["TEST_INSTALLED"] = self.main[:7]
        self.run_update("--check", status=1)
        self.run_update()
        self.assertEqual(self.calls("rpmbuild"), [])
        self.assertEqual(self.calls("sudo"), [])

    def test_local_build_uses_exact_branch_revision(self):
        self.run_update("--branch", "fix/nvidia", "--build", "--jobs", "2")
        built = self.calls("rpmbuild")
        self.assertEqual(len(built), 1)
        self.assertEqual(built[0]["sha"], self.nvidia)
        self.assertEqual(built[0]["jobs"], "2")
        self.assertIn("Name:           vela-git", built[0]["spec"])
        self.assertEqual(self.calls("sudo")[0]["args"][:2], ["dnf", "builddep"])
        self.assertIn("vela_version 0.1.0", self.calls("sudo")[0]["args"])
        self.assertEqual(self.calls("sudo")[0]["args"][-1].split("/")[-1], "vela.spec")
        self.assertEqual(self.calls("sudo")[1]["args"][:3], ["dnf", "install", "-y"])
        self.assertIn(".g" + self.nvidia[:7] + ".fc44.x86_64.rpm",
                      self.calls("sudo")[1]["args"][-1])
        self.assertEqual(len(list((self.mirror.parent / "packages").glob("*.rpm"))), 1)

    def test_force_same_commit_uses_dnf_reinstall(self):
        self.env["TEST_INSTALLED"] = self.main[:7]
        self.run_update("--force", "--build")
        self.assertEqual(self.calls("sudo")[-1]["args"][:3],
                         ["dnf", "reinstall", "-y"])

    def test_artifact_download_is_signed_and_platform_specific(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.nvidia[:7])
        self.run_update("--branch", "fix/nvidia", "--download")
        downloads = [call for call in self.calls("gh")
                     if call["args"][:2] == ["run", "download"]]
        self.assertEqual(downloads[0]["args"][downloads[0]["args"].index("-n") + 1],
                         "vela-fedora-rpm")
        verification = next(call for call in self.calls("cosign")
                            if call["args"][:1] == ["verify-blob-attestation"])
        self.assertEqual(verification["args"][verification["args"].index("--certificate-github-workflow-sha") + 1],
                         self.nvidia)
        self.assertEqual(self.calls("rpmbuild"), [])
        self.assertEqual(self.calls("sudo")[0]["args"][:3], ["dnf", "install", "-y"])

    def test_bad_workflow_attestation_never_installs_download(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.main[:7],
                        TEST_REJECT_ATTEST="1")
        self.run_update("--download")
        self.assertEqual(self.calls("rpmbuild")[0]["sha"], self.main)
        self.assertEqual(len([c for c in self.calls("sudo")
                              if c["args"][:2] == ["dnf", "install"]]), 1)

    def test_public_release_uses_fedora_manifest_and_checks_provenance(self):
        self.env.update(TEST_RELEASE_SHA=self.main)
        self.run_update("--download")
        curls = self.calls("curl")
        self.assertTrue(any(c["args"][-1].endswith("SHA256SUMS-fedora") for c in curls))
        self.assertEqual(self.calls("rpmbuild"), [])
        verification = next(c for c in self.calls("cosign")
                            if c["args"][:1] == ["verify-blob-attestation"])
        self.assertEqual(verification["args"][verification["args"].index("--certificate-github-workflow-ref") + 1],
                         "refs/heads/main")

    def test_bad_public_release_checksum_falls_back_to_local_build(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_BAD_RELEASE_DIGEST="1")
        self.run_update("--download")
        self.assertEqual(self.calls("rpmbuild")[0]["sha"], self.main)
        self.assertEqual(len([c for c in self.calls("sudo")
                              if c["args"][:2] == ["dnf", "install"]]), 1)

    def test_fedora_44_is_supported(self):
        self.env["TEST_FEDORA_RELEASE"] = "44"
        self.run_update("--check")
        self.assertEqual(self.calls("rpmbuild"), [])
        self.assertEqual(self.calls("sudo"), [])

    def test_reject_unsupported_fedora_release(self):
        self.env["TEST_FEDORA_RELEASE"] = "45"
        result = self.run_update("--check", status=1)
        self.assertIn("Fedora 44", result.stderr)
        self.assertEqual(self.calls("rpmbuild"), [])
        self.assertEqual(self.calls("sudo"), [])

    def test_reject_dnf_based_fedora_derivative(self):
        # DNF/RPM and a Fedora macro are not enough to allow the RPM backend.
        self.os_release.write_text("ID=nobara\nID_LIKE=fedora\n")
        self.env["TEST_FEDORA_RELEASE"] = "44"
        result = self.run_update("--check", status=1)
        self.assertIn("unsupported distribution", result.stderr)
        self.assertEqual(self.calls("rpmbuild"), [])
        self.assertEqual(self.calls("sudo"), [])

    def test_local_build_selects_main_rpm_not_debug_subpackages(self):
        self.run_update("--build")
        installed = [c for c in self.calls("sudo") if c["args"][:2] == ["dnf", "install"]]
        self.assertEqual(len(installed), 1)
        self.assertNotIn("debuginfo", installed[0]["args"][-1])
        self.assertNotIn("debugsource", installed[0]["args"][-1])
        self.assertEqual(len(list((self.mirror.parent / "packages").glob("*.rpm"))), 1)

    def test_project_version_controls_rpm_version(self):
        import re
        version = re.search(r"(?m)^\s*VERSION\s+([0-9]+(?:\.[0-9]+)+)\s*$",
                            (ROOT / "CMakeLists.txt").read_text()).group(1)
        self.assertIn("Version:        %{vela_version}", self.spec.read_text())
        self.run_update("--build")
        self.assertIn("vela_version " + version, self.calls("rpmbuild")[0]["args"])

    def test_missing_commit_does_not_install(self):
        self.run_update("--commit", "0" * 40, status=1)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("rpmbuild"), [])
