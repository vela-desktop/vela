#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline updater contracts with real Git and simulated package/CI tools."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
UPDATER = ROOT / "packaging/arch/vela-update"
HTTPS_SOURCE = "https://github.com/vela-desktop/vela.git"
GIT_STUB = r'''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys
args = sys.argv[1:]
real_git = os.environ["TEST_REAL_GIT"]
command = args[2:] if args[:1] == ["-C"] else args
url = None
operation = None
if command[:1] == ["clone"]:
    operation, url = "clone", args[-2]
elif command[:1] == ["fetch"] or command[:2] == ["remote", "update"]:
    operation = "fetch" if command[0] == "fetch" else "update"
    directory = args[args.index("-C") + 1]
    url = subprocess.check_output(
        [real_git, "-C", directory, "config", "remote.origin.url"], text=True).strip()
if url is not None:
    with open(os.environ["TEST_CALLS"], "a") as log:
        log.write(json.dumps(dict(tool="git", args=args, operation=operation, url=url)) + "\n")
    # No network or credentials: only the public HTTPS endpoint (mapped to
    # our real local repository below) and paths inside this fixture exist.
    local = url.removeprefix("file://").split("#", 1)[0]
    local_path = pathlib.Path(local).resolve()
    allowed_local = local_path.is_dir() and local_path.is_relative_to(
        pathlib.Path(os.environ["TEST_DIRECTORY"]))
    if url != os.environ["TEST_HTTPS_SOURCE"] and not allowed_local:
        sys.exit("Unexpected repository transport: " + url)
    if os.environ.get("TEST_FAIL_FETCH") and operation in ("clone", "update", "fetch"):
        sys.exit(1)
rewrite = "url." + pathlib.Path(os.environ["TEST_SOURCE"]).as_uri() + ".insteadOf=" + os.environ["TEST_HTTPS_SOURCE"]
sys.exit(subprocess.call([real_git, *(["-c", rewrite] if url is not None else []), *args]))
'''
STUB = r'''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
def record(**extra):
    with open(os.environ["TEST_CALLS"], "a") as log:
        log.write(json.dumps(dict(tool=name, args=args, **extra)) + "\n")
if name == "pacman":
    if not os.environ.get("TEST_INSTALLED"):
        sys.exit(1)
    print("vela-git 0.1.0.r1.g" + os.environ["TEST_INSTALLED"] + "-1")
elif name == "makepkg":
    if os.environ.get("TEST_FAIL_BUILD"):
        record()
        sys.exit(1)
    # Read the real PKGBUILD's VCS URL and check out what it asks makepkg for.
    url = subprocess.check_output(
        ["bash", "-c", 'source ./PKGBUILD; printf "%s" "${source[0]}"'], text=True)
    url = url.split("::git+", 1)[1]
    repo, fragment = url.split("#commit=", 1)
    subprocess.run(["git", "clone", "--quiet", "--mirror", repo, "sources.git"], check=True)
    subprocess.run(["git", "clone", "--quiet", "--shared", "sources.git", "checkout"], check=True)
    subprocess.run(["git", "-C", "checkout", "checkout", "--quiet", "--detach", fragment], check=True)
    sha = subprocess.check_output(["git", "-C", "checkout", "rev-parse", "HEAD"], text=True).strip()
    record(sha=sha, url=url, pkgbuild=pathlib.Path("PKGBUILD").read_text(),
           makeflags=os.environ.get("MAKEFLAGS"))
    if os.environ.get("TEST_FAIL_BUILD_INSTALL"):
        dest = pathlib.Path(os.environ["PKGDEST"])
        dest.mkdir(parents=True, exist_ok=True)
        (dest / ("vela-git-0.1.0.r1.g" + sha[:7] + "-1-x86_64.pkg.tar.zst")).touch()
        sys.exit(1)
elif name == "gh":
    record()
    if args[:2] == ["auth", "status"]:
        sys.exit(0 if os.environ.get("TEST_CI_RUN") else 1)
    if args[:2] == ["run", "list"]:
        print(os.environ["TEST_CI_RUN"])
    elif args[0] == "api":
        print("99")
    elif args[:2] == ["run", "download"]:
        if os.environ.get("TEST_FAIL_DOWNLOAD"):
            sys.exit(1)
        dest = pathlib.Path(args[args.index("-D") + 1])
        dest.mkdir(parents=True)
        sha = os.environ["TEST_DOWNLOAD_SHA"]
        (dest / ("vela-git-0.1.0.r1.g" + sha + "-1-x86_64.pkg.tar.zst")).touch()
elif name == "sudo":
    record()
    sys.exit(int(os.environ.get("TEST_FAIL_INSTALL", "0")))
else:
    sys.exit("Unexpected tool: " + name)
'''


class UpdateFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vela-update-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.source = self.directory / "source"
        self.source.mkdir()
        bin_dir = self.directory / "bin"
        bin_dir.mkdir()
        for name in ("pacman", "makepkg", "gh", "sudo", "git"):
            stub = bin_dir / name
            stub.write_text(GIT_STUB if name == "git" else STUB)
            stub.chmod(0o755)
        self.calls_file = self.directory / "calls.jsonl"
        # Ignore the developer's Git rewrites, SSH keys, credentials and test
        # controls. Git is restricted to file transport even if a stub errs.
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("GIT_", "TEST_", "VELA_", "SSH_", "GH_"))}
        self.env.update(PATH=f"{bin_dir}:{os.environ['PATH']}",
                        HOME=str(self.directory / "home"),
                        XDG_CONFIG_HOME=str(self.directory / "config"),
                        XDG_CACHE_HOME=str(self.directory / "cache"),
                        GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_ALLOW_PROTOCOL="file", GIT_TERMINAL_PROMPT="0",
                        TEST_CALLS=str(self.calls_file), TEST_REAL_GIT=shutil.which("git"),
                        TEST_SOURCE=str(self.source), TEST_DIRECTORY=str(self.directory),
                        TEST_HTTPS_SOURCE=HTTPS_SOURCE)
        self.config = self.directory / "config/vela/updates.conf"
        self.mirror = self.directory / "cache/vela-update/vela.git"
        self.git("init", "--quiet", "--initial-branch=main")
        self.git("config", "user.name", "Updater Test")
        self.git("config", "user.email", "test@example.invalid")
        self.git("remote", "add", "origin", HTTPS_SOURCE)
        packaging = self.source / "packaging/arch"
        packaging.mkdir(parents=True)
        self.pkgbuild = packaging / "PKGBUILD"
        self.pkgbuild.write_text((ROOT / "packaging/arch/PKGBUILD").read_text())
        self.base = self.commit("base")
        self.pkgbuild.write_text(self.pkgbuild.read_text() + "\n# main package\n")
        self.main = self.commit("main update")
        self.git("checkout", "--quiet", "-b", "fix/nvidia", self.base)
        self.pkgbuild.write_text(self.pkgbuild.read_text() + "\n# NVIDIA test package\n")
        self.nvidia = self.commit("NVIDIA fix")
        self.git("checkout", "--quiet", "main")
        self.env.update(TEST_INSTALLED=self.base[:7],
                        TEST_CI_RUN="", TEST_DOWNLOAD_SHA="", TEST_FAIL_DOWNLOAD="")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.source), *args],
                                       env=self.env, text=True, timeout=10).strip()

    def commit(self, message):
        self.git("add", ".")
        self.git("commit", "--quiet", "-m", message)
        return self.git("rev-parse", "HEAD")

    def run_update(self, *args, status=0, source=True):
        source_args = ["--source", str(self.source)] if source else []
        result = subprocess.run(["sh", str(UPDATER), *source_args, *args],
                                env=self.env, input="", text=True, capture_output=True,
                                timeout=30)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result

    def calls(self, tool):
        if not self.calls_file.exists():
            return []
        return [call for line in self.calls_file.read_text().splitlines()
                if (call := json.loads(line))["tool"] == tool]


class UpdateRevision(UpdateFixture):
    def test_default_build_uses_main(self):
        self.run_update("--build")
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_branch_build_and_return_to_main(self):
        result = self.run_update("--branch", "fix/nvidia", "--build")
        build = self.calls("makepkg")[0]
        self.assertEqual(build["sha"], self.nvidia)
        self.assertIn("# NVIDIA test package", build["pkgbuild"])
        self.assertIn("NVIDIA fix", result.stdout)
        self.assertNotIn("main update", result.stdout)
        self.env["TEST_INSTALLED"] = self.nvidia[:7]
        self.run_update("--build")
        self.assertEqual(self.calls("makepkg")[-1]["sha"], self.main)

    def test_commit_build_supports_rollback_and_short_sha(self):
        self.env["TEST_INSTALLED"] = self.main[:7]
        self.run_update("--commit=" + self.base[:7], "--build")
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.base)

    def test_full_commit_can_be_fetched_without_a_branch(self):
        self.run_update("--check")
        self.git("checkout", "--quiet", "--detach", self.base)
        self.pkgbuild.write_text(self.pkgbuild.read_text() + "\n# unreferenced test commit\n")
        detached = self.commit("detached fix")
        self.git("checkout", "--quiet", "main")
        self.run_update("--commit", detached, "--build")
        self.assertEqual(self.calls("makepkg")[0]["sha"], detached)

    def test_branch_check_does_not_install(self):
        self.run_update("--branch=fix/nvidia", "--check")
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_check_with_ready_artifact_does_not_download_or_install(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.main[:7])
        self.run_update("--check")
        self.assertFalse(any(call["args"][:2] == ["run", "download"] for call in self.calls("gh")))
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_build_skips_ready_artifact_and_respects_jobs(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.main[:7])
        self.run_update("--build", "--jobs", "2")
        self.assertEqual(self.calls("gh") + self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)
        self.assertEqual(self.calls("makepkg")[0]["makeflags"], "-j2")

    def test_up_to_date_default_does_not_install(self):
        self.env["TEST_INSTALLED"] = self.main[:7]
        self.run_update("--check", status=1)
        self.run_update()
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_already_installed_selected_commit(self):
        self.env["TEST_INSTALLED"] = self.nvidia[:7]
        self.run_update("--commit", self.nvidia, "--check", status=1)
        self.assertEqual(self.calls("makepkg"), [])
        self.run_update("--commit", self.nvidia, "--force", "--build")
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.nvidia)

    def test_download_uses_selected_commit(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.nvidia[:7])
        self.run_update("--branch", "fix/nvidia", "--download")
        listing = next(call for call in self.calls("gh") if call["args"][:2] == ["run", "list"])
        self.assertEqual(listing["args"][listing["args"].index("-c") + 1], self.nvidia)
        self.assertIn(".g" + self.nvidia[:7] + "-", self.calls("sudo")[0]["args"][-1])
        self.assertEqual(self.calls("makepkg"), [])

    def test_failed_download_builds_selected_commit(self):
        self.env.update(TEST_CI_RUN="123", TEST_FAIL_DOWNLOAD="1")
        self.run_update("--commit", self.nvidia, "--download")
        self.assertTrue(any(call["args"][:2] == ["run", "download"] for call in self.calls("gh")))
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.nvidia)

    def test_wrong_artifact_never_installed(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.main[:7])
        self.run_update("--branch", "fix/nvidia", "--download")
        self.assertTrue(any(call["args"][:2] == ["run", "download"] for call in self.calls("gh")))
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.nvidia)

    def test_missing_revision_never_installed(self):
        for args in (("--branch", "missing"), ("--commit", "0" * 40)):
            with self.subTest(args=args):
                self.run_update(*args, status=1)
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_invalid_options_never_installed(self):
        for args in (("--branch",), ("--commit",), ("--branch=",), ("--commit=",),
                     ("--branch", "../main"), ("--commit", "HEAD"),
                     ("--branch", "main", "--commit", self.main)):
            with self.subTest(args=args):
                self.run_update(*args, status=2)
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])


class UpdateSources(UpdateFixture):
    def write_config(self, source):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        self.config.write_text(f"source={source}\n")

    def assert_https_transport(self):
        calls = self.calls("git")
        self.assertTrue(calls, "No Git transport was exercised")
        self.assertTrue(all(call["url"] == HTTPS_SOURCE for call in calls), calls)
        origin = subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip()
        self.assertEqual(origin, HTTPS_SOURCE)

    def test_fresh_install_checks_public_https_without_config_or_credentials(self):
        result = self.run_update("--check", source=False)
        self.assertIn(HTTPS_SOURCE, result.stdout)
        self.assert_https_transport()
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_cached_update_fetches_new_commits_over_https(self):
        self.run_update("--check", source=False)
        self.pkgbuild.write_text(self.pkgbuild.read_text() + "\n# new update\n")
        latest = self.commit("new public update")
        result = self.run_update("--check", source=False)
        self.assertIn(latest, result.stdout)
        self.assertEqual([call["operation"] for call in self.calls("git")], ["clone", "update"])
        self.assert_https_transport()

    def test_explicit_https_source_is_remembered(self):
        self.run_update("--source", HTTPS_SOURCE, "--check", source=False)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.run_update("--check", source=False)
        self.assert_https_transport()

    def test_explicit_source_overrides_saved_local_source(self):
        self.write_config(str(self.source))
        self.run_update("--source", HTTPS_SOURCE, "--check", source=False)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.assert_https_transport()

    def test_empty_saved_source_falls_back_to_https(self):
        self.write_config("")
        self.run_update("--check", source=False)
        self.assert_https_transport()

    def test_current_config_takes_precedence_over_legacy_config(self):
        self.write_config(HTTPS_SOURCE)
        self.config.with_name("aggiornamenti.conf").write_text(f"sorgente={self.source}\n")
        self.run_update("--check", source=False)
        self.assert_https_transport()

    def test_changing_cached_source_reclones_requested_repository(self):
        self.run_update("--check")
        self.run_update("--source", HTTPS_SOURCE, "--check", source=False)
        self.assertEqual([call["operation"] for call in self.calls("git")], ["clone", "clone"])
        self.assertEqual(self.calls("git")[-1]["url"], HTTPS_SOURCE)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")

    def test_saved_old_https_source_is_migrated(self):
        self.write_config("https://github.com/BrandoDev/vela.git")
        self.run_update("--check", source=False)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.assert_https_transport()

    def test_explicit_old_https_source_is_migrated(self):
        self.run_update("--source", "https://github.com/BrandoDev/vela.git",
                        "--check", source=False)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.assert_https_transport()

    def test_old_https_mirror_is_repointed_without_reclone(self):
        self.run_update("--check", source=False)
        subprocess.run(["git", "-C", str(self.mirror), "remote", "set-url",
                        "origin", "https://github.com/BrandoDev/vela.git"],
                       env=self.env, check=True)
        self.run_update("--check", source=False)
        self.assertEqual([call["operation"] for call in self.calls("git")],
                         ["clone", "update"])
        self.assert_https_transport()

    def test_explicit_github_ssh_sources_are_saved_as_https(self):
        for source in ("ssh://git@github.com/BrandoDev/vela.git", "git@github.com:BrandoDev/vela.git"):
            with self.subTest(source=source):
                self.run_update("--source", source, "--check", source=False)
                self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
                self.assert_https_transport()

    def test_remembered_github_ssh_sources_are_migrated(self):
        for source in ("ssh://git@github.com/BrandoDev/vela.git", "git@github.com:BrandoDev/vela.git"):
            with self.subTest(source=source):
                self.write_config(source)
                self.run_update("--check", source=False)
                self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
                self.assert_https_transport()

    def test_legacy_config_is_migrated_to_https(self):
        self.config.parent.mkdir(parents=True)
        legacy = self.config.with_name("aggiornamenti.conf")
        legacy.write_text("sorgente=git@github.com:BrandoDev/vela.git\n")
        self.run_update("--check", source=False)
        self.assertFalse(legacy.exists())
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.assert_https_transport()

    def test_old_ssh_mirror_is_migrated_without_recloning(self):
        self.run_update("--check", source=False)
        for index, source in enumerate(("ssh://git@github.com/BrandoDev/vela.git",
                                        "git@github.com:BrandoDev/vela.git"), start=1):
            with self.subTest(source=source):
                subprocess.run(["git", "-C", str(self.mirror), "remote", "set-url", "origin",
                                source], env=self.env, check=True)
                self.run_update("--check", source=False)
                self.assertEqual([call["operation"] for call in self.calls("git")],
                                 ["clone"] + ["update"] * index)
                self.assert_https_transport()

    def test_old_origin_uses_org_for_ci_artifacts(self):
        self.git("remote", "set-url", "origin", "git@github.com:BrandoDev/vela.git")
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.main[:7])
        self.run_update("--download")
        listing = next(call for call in self.calls("gh")
                       if call["args"][:2] == ["run", "list"])
        self.assertEqual(listing["args"][listing["args"].index("-R") + 1],
                         "vela-desktop/vela")

    def test_local_copy_with_ssh_origin_stays_local(self):
        self.git("remote", "set-url", "origin", "git@github.com:BrandoDev/vela.git")
        self.run_update("--check")
        self.run_update("--check", source=False)
        self.assertEqual(self.config.read_text(), f"source={self.source}\n")
        self.assertTrue(all(call["url"] == str(self.source) for call in self.calls("git")))

    def test_fresh_default_build_uses_https_and_pinned_local_mirror(self):
        self.run_update("--build", source=False)
        build = self.calls("makepkg")[0]
        self.assertEqual(build["sha"], self.main)
        self.assertEqual(build["url"], f"{self.mirror.as_uri()}#commit={self.main}")
        self.assertEqual(self.calls("git")[0]["url"], HTTPS_SOURCE)
        self.assertEqual([call["url"] for call in self.calls("git")],
                         [HTTPS_SOURCE, self.mirror.as_uri(), "sources.git"])

    def test_public_download_uses_https_source_and_correct_repository(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.main[:7])
        self.run_update("--download", source=False)
        self.assert_https_transport()
        listing = next(call for call in self.calls("gh") if call["args"][:2] == ["run", "list"])
        self.assertEqual(listing["args"][listing["args"].index("-R") + 1], "vela-desktop/vela")
        self.assertEqual(self.calls("makepkg"), [])
        self.assertEqual(len(self.calls("sudo")), 1)

    def test_failed_fetch_never_builds_or_installs(self):
        self.env["TEST_FAIL_FETCH"] = "1"
        result = self.run_update("--build", source=False, status=1)
        self.assertIn("can't download Vela", result.stderr)
        self.assertNotIn("SSH key", result.stderr)
        self.assertFalse(self.mirror.exists())
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_failed_cached_fetch_never_installs_stale_version(self):
        self.run_update("--check", source=False)
        self.env["TEST_FAIL_FETCH"] = "1"
        self.run_update("--build", source=False, status=1)
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])
        self.assertTrue(self.mirror.exists())


class UpdateFailures(UpdateFixture):
    def test_failed_build_reports_failure(self):
        self.env["TEST_FAIL_BUILD"] = "1"
        result = self.run_update("--build", status=1)
        self.assertIn("the update failed", result.stderr)
        self.assertEqual(self.calls("sudo"), [])

    def test_built_package_with_failed_install_is_kept_for_retry(self):
        self.env["TEST_FAIL_BUILD_INSTALL"] = "1"
        result = self.run_update("--build", status=1)
        self.assertIn("the package is ready but wasn't installed", result.stderr)
        self.assertEqual(len(list((self.mirror.parent / "packages").glob("*.pkg.tar.zst"))), 1)

    def test_failed_download_install_is_reported_and_package_kept(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.main[:7], TEST_FAIL_INSTALL="1")
        result = self.run_update("--download", status=1)
        self.assertIn("downloaded but not installed", result.stderr)
        self.assertEqual(self.calls("makepkg"), [])
        self.assertEqual(len(list((self.mirror.parent / "packages").glob("*.pkg.tar.zst"))), 1)


class PackageSources(unittest.TestCase):
    def package_source(self, override=None):
        env = {key: value for key, value in os.environ.items() if key != "VELA_GIT_URL"}
        if override is not None:
            env["VELA_GIT_URL"] = override
        return subprocess.check_output(
            ["bash", "-c", 'source "$1"; printf "%s" "${source[0]}"', "bash",
             str(ROOT / "packaging/arch/PKGBUILD")], env=env, text=True, timeout=10)

    def test_default_package_source_is_public_https(self):
        self.assertEqual(self.package_source(), f"vela::git+{HTTPS_SOURCE}")

    def test_empty_override_uses_public_https(self):
        self.assertEqual(self.package_source(""), f"vela::git+{HTTPS_SOURCE}")

    def test_local_commit_override_is_preserved(self):
        source = "file:///tmp/local-vela#commit=" + "a" * 40
        self.assertEqual(self.package_source(source), "vela::git+" + source)


if __name__ == "__main__":
    unittest.main()
