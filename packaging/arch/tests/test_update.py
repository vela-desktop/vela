#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline updater contracts with real Git and simulated package/CI tools."""

import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
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
MV_STUB = r'''#!/usr/bin/env python3
import os, sys
if (os.environ.get("TEST_FAIL_CONFIG_MV") == "1" and
        sys.argv[-1] == os.environ["TEST_CONFIG_DEST"]):
    sys.exit(1)
os.execv(os.environ["TEST_REAL_MV"], [os.environ["TEST_REAL_MV"], *sys.argv[1:]])
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
    if os.environ.get("TEST_PAUSE_BUILD"):
        import time
        marker = pathlib.Path(os.environ["TEST_BUILD_MARKER"])
        release = pathlib.Path(os.environ["TEST_BUILD_RELEASE"])
        if os.environ.get("TEST_SPAWN_BUILD_CHILD"):
            # Model a long-running compiler spawned by makepkg. A signal
            # delivered only to makepkg will never reach this process.
            child_code = """
import os, pathlib, signal, time
ready = pathlib.Path(os.environ["TEST_BUILD_CHILD_READY"])
signalled = pathlib.Path(os.environ["TEST_BUILD_CHILD_SIGNALLED"])
def stop(_signum, _frame):
    signalled.touch()
    raise SystemExit(0)
signal.signal(signal.SIGTERM, stop)
ready.write_text(str(os.getpid()))
while True:
    time.sleep(0.025)
"""
            subprocess.Popen([sys.executable, "-c", child_code])
            child_ready = pathlib.Path(os.environ["TEST_BUILD_CHILD_READY"])
            until = time.monotonic() + 5
            while not child_ready.exists() and time.monotonic() < until:
                time.sleep(0.025)
            if not child_ready.exists():
                sys.exit("Build child failed to start")
        marker.touch()
        end = time.monotonic() + 15
        while not release.exists() and time.monotonic() < end:
            time.sleep(0.025)
        if not release.exists():
            sys.exit("Timed out waiting for test build release")
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
        dest.mkdir(parents=True, exist_ok=True)
        sha = os.environ["TEST_DOWNLOAD_SHA"]
        (dest / ("vela-git-0.1.0.r1.g" + sha + "-1-x86_64.pkg.tar.zst")).touch()
elif name == "curl":
    record()
    import hashlib
    sha = os.environ.get("TEST_RELEASE_SHA")
    if not sha:
        sys.exit(22)  # no public release for this fixture revision
    url = args[-1]
    prefix = "https://github.com/vela-desktop/vela/releases/download/alpha-" + sha + "/"
    if not url.startswith(prefix):
        sys.exit("Unexpected public release URL: " + url)
    output = pathlib.Path(args[args.index("-o") + 1])
    advertised = os.environ.get("TEST_RELEASE_PKG_SHA") or sha
    package = "vela-git-0.1.0.r1.g" + advertised[:7] + "-1-x86_64.pkg.tar.zst"
    data = ("package for " + sha).encode()
    digest = hashlib.sha256(data).hexdigest()
    if os.environ.get("TEST_BAD_RELEASE_DIGEST"):
        digest = "f" * 64
    if url == prefix + "SHA256SUMS":
        output.write_text(digest + "  " + package + "\n")
    elif url == prefix + package and not os.environ.get("TEST_FAIL_RELEASE_DOWNLOAD"):
        output.write_bytes(data + (b"corrupted" if os.environ.get("TEST_CORRUPT_RELEASE") else b""))
    else:
        sys.exit(22)
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
        for name in ("pacman", "makepkg", "gh", "sudo", "curl", "git", "mv"):
            stub = bin_dir / name
            stub.write_text(GIT_STUB if name == "git" else MV_STUB if name == "mv" else STUB)
            stub.chmod(0o755)
        # Installed/updater entry point is outside a checkout: only the
        # explicit bootstrap test below runs the updater from a Git checkout.
        self.update_script = bin_dir / "vela-update-under-test"
        shutil.copyfile(UPDATER, self.update_script)
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
                        TEST_HTTPS_SOURCE=HTTPS_SOURCE,
                        TEST_REAL_MV=shutil.which("mv"),
                        TEST_CONFIG_DEST=str(self.directory / "config/vela/updates.conf"))
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
        shutil.copyfile(UPDATER, packaging / "vela-update")
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
        result = subprocess.run(["sh", str(self.update_script), *source_args, *args],
                                env=self.env, input="", text=True, capture_output=True,
                                timeout=30)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result

    def calls(self, tool):
        if not self.calls_file.exists():
            return []
        return [call for line in self.calls_file.read_text().splitlines()
                if (call := json.loads(line))["tool"] == tool]

    def start_paused_build(self):
        self.env["TEST_PAUSE_BUILD"] = "1"
        marker = self.directory / "build-started"
        release = self.directory / "build-release"
        self.env["TEST_BUILD_MARKER"] = str(marker)
        self.env["TEST_BUILD_RELEASE"] = str(release)
        proc = subprocess.Popen(
            ["sh", str(self.update_script), "--source", str(self.source), "--build"],
            env=self.env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: proc.kill() if proc.poll() is None else None)
        limit = time.monotonic() + 12
        while not marker.exists() and proc.poll() is None and time.monotonic() < limit:
            time.sleep(0.025)
        self.assertTrue(marker.exists(), "Updater did not reach makepkg")
        return proc, release


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
                     ("--branch", "main", "--commit", self.main),
                     ("--jobs",), ("--jobs=",), ("--source",), ("--source=",)):
            with self.subTest(args=args):
                self.run_update(*args, status=2)
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])


class UpdateSources(UpdateFixture):
    def test_checkout_bootstrap_reuses_git_objects_and_tracks_remote(self):
        result = subprocess.run(
            ["sh", str(self.source / "packaging/arch/vela-update"), "--check"],
            env=self.env, input="", text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Reusing Git objects from the checkout", result.stdout)
        calls = self.calls("git")
        self.assertEqual([(call["operation"], call["url"]) for call in calls],
                         [("clone", str(self.source)), ("update", HTTPS_SOURCE)])
        self.assertFalse(self.config.exists(), "Bootstrap must not remember the local checkout")
        origin = subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip()
        self.assertEqual(origin, HTTPS_SOURCE)
        self.pkgbuild.write_text(self.pkgbuild.read_text() + "\n# next update\n")
        new_sha = self.commit("post-bootstrap update")
        checked = self.run_update("--check", source=False)
        self.assertIn(new_sha, checked.stdout)
        self.assertEqual([call["operation"] for call in self.calls("git")],
                         ["clone", "update", "update"])

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

    def test_failed_source_switch_preserves_mirror_and_config(self):
        self.run_update("--check")
        original = subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip()
        self.run_update("--source", str(self.directory / "nonexistent"),
                        "--check", source=False, status=1)
        self.assertEqual(self.config.read_text(), f"source={self.source}\n")
        self.assertEqual(subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip(), original)
        self.run_update("--check", source=False)

    def test_missing_branch_on_new_source_preserves_old_mirror(self):
        self.run_update("--check")
        alternate = self.directory / "alternate"
        shutil.copytree(self.source, alternate)
        self.run_update("--source", str(alternate), "--branch", "nonexistent",
                        "--check", source=False, status=1)
        self.assertEqual(self.config.read_text(), f"source={self.source}\n")
        origin = subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip()
        self.assertEqual(origin, str(self.source))

    def test_config_failure_restores_previous_mirror(self):
        self.run_update("--check")
        original_origin = subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip()
        alternate = self.directory / "alternate"
        shutil.copytree(self.source, alternate)
        self.env["TEST_FAIL_CONFIG_MV"] = "1"
        try:
            self.run_update("--source", str(alternate), "--check",
                            source=False, status=1)
        finally:
            self.env.pop("TEST_FAIL_CONFIG_MV")
        self.assertEqual(self.config.read_text(), f"source={self.source}\n")
        restored_origin = subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip()
        self.assertEqual(restored_origin, original_origin)
        self.assertFalse(list(self.mirror.parent.glob("vela-backup.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))
        self.run_update("--check", source=False)

    def test_initial_config_failure_removes_uncommitted_mirror(self):
        self.env["TEST_FAIL_CONFIG_MV"] = "1"
        try:
            self.run_update("--source", str(self.source), "--check", source=False, status=1)
        finally:
            self.env.pop("TEST_FAIL_CONFIG_MV")
        self.assertFalse(self.mirror.exists())
        self.assertFalse(self.config.exists())
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))

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


class UpdatePublicReleases(UpdateFixture):
    def test_public_package_installs_without_gh_login_or_build(self):
        self.env["TEST_RELEASE_SHA"] = self.main
        self.run_update("--download", source=False)
        self.assertEqual(len(self.calls("curl")), 2)
        self.assertTrue(all("/alpha-" + self.main + "/" in c["args"][-1]
                            for c in self.calls("curl")))
        self.assertEqual(self.calls("gh"), [])
        self.assertEqual(self.calls("makepkg"), [])
        self.assertEqual(len(self.calls("sudo")), 1)
        self.assertIn(".g" + self.main[:7] + "-", self.calls("sudo")[0]["args"][-1])

    def test_check_only_fetches_manifest(self):
        self.env["TEST_RELEASE_SHA"] = self.main
        result = self.run_update("--check", source=False)
        self.assertIn("Public alpha package available", result.stdout)
        self.assertEqual(len(self.calls("curl")), 1)
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_force_local_build_skips_public_release(self):
        self.env["TEST_RELEASE_SHA"] = self.main
        self.run_update("--build", source=False)
        self.assertEqual(self.calls("curl"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_wrong_release_commit_is_not_downloaded(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_RELEASE_PKG_SHA=self.nvidia)
        self.run_update("--download", source=False)
        self.assertEqual(len(self.calls("curl")), 1)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_bad_digest_falls_back_to_local_build(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_BAD_RELEASE_DIGEST="1")
        result = self.run_update("--download", source=False)
        self.assertIn("failed checksum verification", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_corrupted_download_falls_back_to_local_build(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_CORRUPT_RELEASE="1")
        self.run_update("--download", source=False)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_absent_release_keeps_build_fallback(self):
        self.run_update("--download", source=False)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)


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


class UpdateConcurrency(UpdateFixture):
    def test_second_invocation_is_rejected_without_touching_build(self):
        proc, release = self.start_paused_build()
        try:
            result = self.run_update("--check", status=1)
            self.assertIn("another update is already running", result.stderr)
            self.assertEqual(self.calls("sudo"), [])
            self.assertTrue(list(self.mirror.parent.glob("build.*")))
        finally:
            release.touch()
            stdout, stderr = proc.communicate(timeout=30)
        self.assertEqual(proc.returncode, 0, stdout + stderr)
        self.assertFalse(list(self.mirror.parent.glob("build.*")))

    def test_interruption_signals_makepkg_descendants(self):
        self.env["TEST_SPAWN_BUILD_CHILD"] = "1"
        child_ready = self.directory / "build-child-ready"
        child_signalled = self.directory / "build-child-signalled"
        self.env["TEST_BUILD_CHILD_READY"] = str(child_ready)
        self.env["TEST_BUILD_CHILD_SIGNALLED"] = str(child_signalled)
        proc, release = self.start_paused_build()
        self.assertTrue(child_ready.exists(), "Expected a real build child")
        try:
            proc.send_signal(signal.SIGTERM)
            stdout, stderr = proc.communicate(timeout=8)
        finally:
            release.touch()
        self.assertEqual(proc.returncode, 143, stdout + stderr)
        self.assertTrue(child_signalled.exists(),
                        "The SIGTERM handler did not reach the compiler child")
        self.assertFalse(list(self.mirror.parent.glob("build.*")))
        self.env.pop("TEST_PAUSE_BUILD", None)
        self.run_update("--check")

    def test_interrupted_build_cleans_temp_directories_and_keeps_mirror(self):
        proc, release = self.start_paused_build()
        proc.send_signal(signal.SIGTERM)
        try:
            # A hung makepkg never releases its marker: SIGTERM must still
            # terminate the updater promptly without waiting for it.
            stdout, stderr = proc.communicate(timeout=8)
        finally:
            release.touch()
        self.assertEqual(proc.returncode, 143, stdout + stderr)
        self.assertTrue(self.mirror.is_dir())
        self.assertFalse(list(self.mirror.parent.glob("build.*")))
        self.assertFalse(list(self.mirror.parent.glob("download.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))
        self.env.pop("TEST_PAUSE_BUILD", None)
        self.run_update("--check")


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
