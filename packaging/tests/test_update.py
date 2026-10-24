#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline updater contracts with real Git and simulated package/CI tools."""

import fcntl
import json
import os
import pty
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import termios
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
UPDATER = ROOT / "packaging/vela-update"
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
        sys.exit("simulated Git transport failure")
rewrite = "url." + pathlib.Path(os.environ["TEST_SOURCE"]).as_uri() + ".insteadOf=" + os.environ["TEST_HTTPS_SOURCE"]
sys.exit(subprocess.call([real_git, *(["-c", rewrite] if url is not None else []), *args]))
'''
MV_STUB = r'''#!/usr/bin/env python3
import os, pathlib, signal, subprocess, sys
if (os.environ.get("TEST_FAIL_CONFIG_MV") == "1" and
        sys.argv[-1] == os.environ["TEST_CONFIG_DEST"]):
    sys.exit(1)
mirror_swap = (os.environ.get("TEST_INTERRUPT_MIRROR_SWAP") == "1"
               and sys.argv[-1] == os.environ["TEST_MIRROR_DEST"]
               and pathlib.Path(sys.argv[-2]).name.startswith("vela-mirror."))
old_mirror_swap = (os.environ.get("TEST_INTERRUPT_OLD_MIRROR_SWAP") == "1"
                   and sys.argv[-2] == os.environ["TEST_MIRROR_DEST"]
                   and pathlib.Path(sys.argv[-1]).name.startswith("vela-corrupt."))
config_commit = (os.environ.get("TEST_INTERRUPT_CONFIG_COMMIT") == "1"
                 and sys.argv[-1] == os.environ["TEST_CONFIG_DEST"])
if old_mirror_swap:
    os.kill(os.getppid(), signal.SIGTERM)
    sys.exit(0)  # Pretend mv returned normally after the parent was signaled
if mirror_swap or config_commit:
    subprocess.run([os.environ["TEST_REAL_MV"], *sys.argv[1:]], check=True)
    os.kill(os.getppid(), signal.SIGTERM)
    sys.exit(0)
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
    if args == ["-Qq", "base-devel"]:
        sys.exit(0 if os.environ.get("TEST_BASE_DEVEL", "1") == "1" else 1)
    if not os.environ.get("TEST_INSTALLED"):
        sys.exit(1)
    print("vela-git 0.1.0.r1.g" + os.environ["TEST_INSTALLED"] + "-1")
elif name == "rpm":
    if args[:2] == ["--eval", "%{?fedora}"]:
        print(os.environ.get("TEST_FEDORA_RELEASE", "44"))
        sys.exit(0)
    if not os.environ.get("TEST_INSTALLED"):
        sys.exit(1)
    print("0.1.0-1.1.g" + os.environ["TEST_INSTALLED"] + ".fc44")
elif name == "rpmbuild":
    values = {}
    for idx, value in enumerate(args[:-1]):
        if value == "--define":
            key, _, entry = args[idx + 1].partition(" ")
            values[key] = entry
    sha = values["vela_commit"]
    topdir = pathlib.Path(values["_topdir"])
    version = values["vela_version"]
    assert (topdir / ("SOURCES/vela-" + version + ".tar.gz")).is_file()
    assert (topdir / "SPECS/vela.spec").is_file()
    record(sha=sha, jobs=os.environ.get("RPM_BUILD_NCPUS"),
           spec=(topdir / "SPECS/vela.spec").read_text())
    if os.environ.get("TEST_FAIL_BUILD"):
        sys.exit(1)
    rpm_dir = topdir / "RPMS/x86_64"
    rpm_dir.mkdir(parents=True, exist_ok=True)
    (rpm_dir / ("vela-git-" + version + "-1.1.g" + sha[:7] + ".fc44.x86_64.rpm")).touch()
    # Fedora also emits these debug subpackages by default: ignore both.
    for suffix in ("-debuginfo", "-debugsource"):
        (rpm_dir / ("vela-git" + suffix + "-" + version + "-1.1.g" + sha[:7] + ".fc44.x86_64.rpm")).touch()
elif name == "makepkg":
    if os.environ.get("TEST_REQUIRE_TTY"):
        # sudo authenticates through /dev/tty, not necessarily stdin.
        fd = os.open("/dev/tty", os.O_RDONLY | os.O_NONBLOCK)
        os.close(fd)
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
elif name == "cosign":
    record()
    if args[:1] != ["verify-blob-attestation"]:
        sys.exit("Unexpected cosign command")
    sha = args[args.index("--certificate-github-workflow-sha") + 1]
    pkg = pathlib.Path(args[-1])
    if (".g" + sha[:7] + "-" not in pkg.name and
            ".g" + sha[:7] + "." not in pkg.name):
        sys.exit("Attestation commit mismatch")
    if args[args.index("--certificate-github-workflow-repository") + 1] != "vela-desktop/vela":
        sys.exit("Unexpected repository identity")
    if args[args.index("--certificate-oidc-issuer") + 1] != "https://token.actions.githubusercontent.com":
        sys.exit("Unexpected OIDC issuer")
    if os.environ.get("TEST_REJECT_ATTEST") or os.environ.get("TEST_WRONG_ATTEST_SHA"):
        sys.exit("simulated cosign attestation rejection")
    if os.environ.get("TEST_REQUIRE_MAIN_REF") and (
        "--certificate-github-workflow-ref" not in args or
        args[args.index("--certificate-github-workflow-ref") + 1] != "refs/heads/main"
    ):
        sys.exit("Expected main source ref")
elif name == "gh":
    record()
    if args[:2] == ["attestation", "verify"]:
        # Simulate the GitHub CLI's verified identity + source commit policy,
        # NOT the underlying Sigstore cryptography. A failure must fail closed.
        if os.environ.get("TEST_REJECT_ATTEST"):
            sys.exit("simulated cosign attestation rejection")
        if args[args.index("--repo") + 1] != "vela-desktop/vela":
            sys.exit("Unexpected attestation repository")
        if args[args.index("--signer-workflow") + 1] != "vela-desktop/vela/.github/workflows/build.yml":
            sys.exit("Unexpected signer workflow")
        sha = args[args.index("--certificate-github-workflow-sha") + 1]
        if (".g" + sha[:7] + "-" not in pathlib.Path(args[2]).name and
                ".g" + sha[:7] + "." not in pathlib.Path(args[2]).name):
            sys.exit("Attestation source does not match the artifact")
        if os.environ.get("TEST_WRONG_ATTEST_SHA"):
            sys.exit(1)
        if "--deny-self-hosted-runners" not in args:
            sys.exit("Self-hosted runners must not be trusted")
        if os.environ.get("TEST_REQUIRE_MAIN_REF") and (
            "--source-ref" not in args or
            args[args.index("--source-ref") + 1] != "refs/heads/main"
        ):
            sys.exit("Expected main source ref")
    elif args[:2] == ["auth", "status"]:
        sys.exit(0 if (os.environ.get("TEST_CI_RUN") or os.environ.get("TEST_CI_RUNS")) else 1)
    if args[:2] == ["run", "list"]:
        # The real gh CLI prints one run ID per line after the jq event filter.
        print((os.environ.get("TEST_CI_RUNS") or os.environ.get("TEST_CI_RUN", "")).replace(",", "\n"))
    elif args[0] == "api":
        missing = os.environ.get("TEST_CI_MISSING_RUNS", "").split(",")
        run_id = args[1].split("/runs/", 1)[1].split("/", 1)[0]
        if run_id not in missing:
            print("99")
    elif args[:2] == ["run", "download"]:
        if os.environ.get("TEST_FAIL_DOWNLOAD"):
            sys.exit(1)
        dest = pathlib.Path(args[args.index("-D") + 1])
        dest.mkdir(parents=True, exist_ok=True)
        sha = os.environ["TEST_DOWNLOAD_SHA"]
        if os.environ.get("TEST_FEDORA"):
            (dest / ("vela-git-0.1.0-1.1.g" + sha + ".fc44.x86_64.rpm")).touch()
        else:
            (dest / ("vela-git-0.1.0.r1.g" + sha + "-1-x86_64.pkg.tar.zst")).touch()
elif name == "curl":
    record()
    import hashlib
    url = args[-1]
    if url.startswith("https://api.github.com/repos/vela-desktop/vela/attestations/sha256:"):
        print(json.dumps({"attestations": [{"bundle_url":
              "https://vela-test.blob.core.windows.net/attestations/bundle.json"}]}))
        sys.exit(0)
    if url == "https://vela-test.blob.core.windows.net/attestations/bundle.json":
        import base64
        digest = os.environ.get("TEST_EXPECTED_DIGEST", "")
        if not digest:
            # Workflow artifacts are empty fake packages in this fixture.
            release_sha = os.environ.get("TEST_RELEASE_SHA")
            digest = hashlib.sha256(("package for " + release_sha).encode() if release_sha else b"").hexdigest()
        runner = "self-hosted" if os.environ.get("TEST_SELF_HOSTED") else "github-hosted"
        statement = {"predicateType": "https://slsa.dev/provenance/v1",
                     "subject": [{"digest": {"sha256": digest}}],
                     "predicate": {"buildDefinition": {"internalParameters":
                                  {"github": {"runner_environment": runner}}}}}
        print(json.dumps({"dsseEnvelope": {"payload":
              base64.b64encode(json.dumps(statement).encode()).decode()}}))
        sys.exit(0)
    import hashlib
    sha = os.environ.get("TEST_RELEASE_SHA")
    if os.environ.get("TEST_RELEASE_TIMEOUT"):
        print("000")  # curl's --write-out HTTP code for a network failure
        sys.exit("simulated release lookup timeout")
    if os.environ.get("TEST_RELEASE_HTTP"):
        code = os.environ["TEST_RELEASE_HTTP"]
        print(code)
        if code != "404":
            print("simulated release lookup HTTP " + code, file=sys.stderr)
        sys.exit(22)  # curl -f uses 22 for all HTTP errors
    if not sha:
        print("404")
        sys.exit(22)  # no public release for this fixture revision
    url = args[-1]
    prefix = "https://github.com/vela-desktop/vela/releases/download/alpha-" + sha + "/"
    if not url.startswith(prefix):
        sys.exit("Unexpected public release URL: " + url)
    output = pathlib.Path(args[args.index("-o") + 1])
    advertised = os.environ.get("TEST_RELEASE_PKG_SHA") or sha
    if os.environ.get("TEST_FEDORA"):
        package = "vela-git-0.1.0-1.1.g" + advertised[:7] + ".fc44.x86_64.rpm"
    else:
        package = "vela-git-0.1.0.r1.g" + advertised[:7] + "-1-x86_64.pkg.tar.zst"
    data = ("package for " + sha).encode()
    digest = hashlib.sha256(data).hexdigest()
    if os.environ.get("TEST_BAD_RELEASE_DIGEST"):
        digest = "f" * 64
    manifest_name = "SHA256SUMS-fedora" if os.environ.get("TEST_FEDORA") else "SHA256SUMS"
    if url == prefix + manifest_name:
        mode = os.environ.get("TEST_BAD_RELEASE_MANIFEST")
        if mode == "invalid-digest":
            digest = "not-a-sha256"
        if mode == "empty":
            content = ""
        elif mode == "multiple":
            content = digest + "  " + package + "\n" + digest + "  " + package + "\n"
        else:
            content = digest + "  " + package + "\n"
        output.write_text(content)
        print("200")
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
        for name in ("pacman", "makepkg", "gh", "cosign", "sudo", "curl", "git", "mv"):
            stub = bin_dir / name
            stub.write_text(GIT_STUB if name == "git" else MV_STUB if name == "mv" else STUB)
            stub.chmod(0o755)
        # Installed/updater entry point is outside a checkout: only the
        # explicit bootstrap test below runs the updater from a Git checkout.
        self.update_script = bin_dir / "vela-update-under-test"
        shutil.copyfile(UPDATER, self.update_script)
        shutil.copyfile(ROOT / "packaging/verify-attestation.py", bin_dir / "verify-attestation.py")
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
                        TEST_CONFIG_DEST=str(self.directory / "config/vela/updates.conf"),
                        TEST_MIRROR_DEST=str(self.directory / "cache/vela-update/vela.git"))
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
        shutil.copyfile(UPDATER, self.source / "packaging/vela-update")
        shutil.copyfile(ROOT / "packaging/verify-attestation.py", self.source / "packaging/verify-attestation.py")
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

    def write_config(self, source):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        self.config.write_text(f"source={source}\n")

    def calls(self, tool):
        if not self.calls_file.exists():
            return []
        return [call for line in self.calls_file.read_text().splitlines()
                if (call := json.loads(line))["tool"] == tool]

    def start_paused_build(self, interactive=False):
        self.env["TEST_PAUSE_BUILD"] = "1"
        marker = self.directory / "build-started"
        release = self.directory / "build-release"
        self.env["TEST_BUILD_MARKER"] = str(marker)
        self.env["TEST_BUILD_RELEASE"] = str(release)
        stdin = subprocess.DEVNULL
        preexec_fn = None
        if interactive:
            master, slave = pty.openpty()
            stdin = slave

            def attach_terminal():
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

            preexec_fn = attach_terminal
        try:
            proc = subprocess.Popen(
                ["sh", str(self.update_script), "--source", str(self.source), "--build"],
                env=self.env, stdin=stdin, preexec_fn=preexec_fn,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        finally:
            if interactive:
                os.close(slave)
        if interactive:
            self.addCleanup(lambda: os.close(master))
        self.addCleanup(lambda: proc.kill() if proc.poll() is None else None)
        limit = time.monotonic() + 12
        while not marker.exists() and proc.poll() is None and time.monotonic() < limit:
            time.sleep(0.025)
        self.assertTrue(marker.exists(), "Updater did not reach makepkg")
        return proc, release


class UpdateRevision(UpdateFixture):
    def test_missing_base_devel_installed_for_local_build(self):
        self.env["TEST_BASE_DEVEL"] = "0"
        self.run_update("--build")
        self.assertTrue(any(c["args"] == ["pacman", "-S", "--needed", "base-devel"]
                            for c in self.calls("sudo")))
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_present_base_devel_not_reinstalled(self):
        self.run_update("--build")
        self.assertFalse(any(c["args"] == ["pacman", "-S", "--needed", "base-devel"]
                             for c in self.calls("sudo")))

    def test_failed_base_devel_install_stops_before_makepkg(self):
        self.env["TEST_BASE_DEVEL"] = "0"
        self.env["TEST_FAIL_INSTALL"] = "1"
        result = self.run_update("--build", status=1)
        self.assertIn("could not install base-devel", result.stderr)
        self.assertEqual(self.calls("makepkg"), [])

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

    def test_artifact_lookup_skips_recent_runs_without_a_package(self):
        self.env.update(TEST_CI_RUNS="303,202,101",
                        TEST_CI_MISSING_RUNS="303,202",
                        TEST_DOWNLOAD_SHA=self.nvidia[:7])
        self.run_update("--branch", "fix/nvidia", "--download")
        calls = self.calls("gh")
        listing = next(c for c in calls if c["args"][:2] == ["run", "list"])
        self.assertEqual(listing["args"][listing["args"].index("-L") + 1], "20")
        self.assertIn("event", listing["args"][listing["args"].index("--json") + 1])
        self.assertIn('select(.event == "push" or .event == "workflow_dispatch")',
                      listing["args"][listing["args"].index("-q") + 1])
        lookups = [c["args"][1] for c in calls if c["args"][0] == "api"]
        self.assertEqual(lookups, [
            f"repos/vela-desktop/vela/actions/runs/{run}/artifacts?per_page=100"
            for run in ("303", "202", "101")
        ])
        download = next(c for c in calls if c["args"][:2] == ["run", "download"])
        self.assertEqual(download["args"][2], "101")
        self.assertEqual(self.calls("makepkg"), [])
        self.assertEqual(self.calls("sudo")[0]["args"][:2], ["pacman", "-U"])

    def test_artifact_lookup_without_matching_run_builds_selected_revision(self):
        self.env.update(TEST_CI_RUNS="303,202",
                        TEST_CI_MISSING_RUNS="303,202")
        self.run_update("--branch", "fix/nvidia", "--download")
        self.assertFalse(any(c["args"][:2] == ["run", "download"] for c in self.calls("gh")))
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.nvidia)

    def test_workflow_artifact_with_invalid_attestation_is_never_installed(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.nvidia[:7],
                        TEST_REJECT_ATTEST="1")
        result = self.run_update("--branch", "fix/nvidia", "--download")
        self.assertIn("provenance verification failed", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.nvidia)
        self.assertEqual(list((self.mirror.parent / "packages").glob("*.pkg.tar.zst")), [])

    def test_workflow_artifact_attestation_failure_shows_reason_and_builds(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.nvidia[:7],
                        TEST_REJECT_ATTEST="1")
        result = self.run_update("--branch", "fix/nvidia", "--download")
        self.assertIn("simulated cosign attestation rejection", result.stderr)
        self.assertIn("provenance verification failed", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.nvidia)

    def test_workflow_artifact_attestation_is_tied_to_selected_sha(self):
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.nvidia[:7])
        self.run_update("--branch", "fix/nvidia", "--download")
        verification = next(call for call in self.calls("cosign")
                            if call["args"][:1] == ["verify-blob-attestation"])
        args = verification["args"]
        self.assertEqual(args[args.index("--certificate-github-workflow-sha") + 1], self.nvidia)
        self.assertNotIn("--certificate-github-workflow-ref", args)  # manual branch packages
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
            ["sh", str(self.source / "packaging/vela-update"), "--check"],
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

    def test_saved_official_https_case_variant_is_canonicalized(self):
        self.write_config("https://github.com/VELA-DESKTOP/Vela.git")
        self.run_update("--check", source=False)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.assert_https_transport()

    def test_explicit_official_https_case_variant_is_canonicalized(self):
        self.run_update("--source", "https://github.com/VELA-DESKTOP/Vela.git",
                        "--check", source=False)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.assert_https_transport()

    def test_official_git_url_aliases_are_canonicalized(self):
        aliases = (
            "https://github.com/vela-desktop/vela",
            "https://github.com/vela-desktop/vela.git",
            "ssh://git@github.com/vela-desktop/vela",
            "ssh://git@github.com/vela-desktop/vela.git",
            "git@github.com:vela-desktop/vela",
            "git@github.com:vela-desktop/vela.git",
            "https://github.com/VeLa-DeSkToP/VeLa.git",
        )
        for url in aliases:
            with self.subTest(url=url):
                self.write_config(url)
                self.run_update("--check", source=False)
                self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
                self.assert_https_transport()

    def test_official_url_without_git_suffix_is_canonicalized(self):
        self.run_update("--source", "https://github.com/vela-desktop/vela",
                        "--check", source=False)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.assert_https_transport()

    def test_official_url_alias_mirror_is_repointed_without_reclone(self):
        self.run_update("--check", source=False)
        subprocess.run(["git", "-C", str(self.mirror), "remote", "set-url",
                        "origin", "https://github.com/vela-desktop/vela"],
                       env=self.env, check=True)
        self.run_update("--check", source=False)
        self.assertEqual([call["operation"] for call in self.calls("git")],
                         ["clone", "update"])
        self.assert_https_transport()

    def test_official_https_case_variant_mirror_is_repointed_without_reclone(self):
        self.run_update("--check", source=False)
        subprocess.run(["git", "-C", str(self.mirror), "remote", "set-url",
                        "origin", "https://github.com/VELA-DESKTOP/Vela.git"],
                       env=self.env, check=True)
        self.run_update("--check", source=False)
        self.assertEqual([call["operation"] for call in self.calls("git")],
                         ["clone", "update"])
        self.assert_https_transport()

    def test_explicit_github_ssh_sources_are_saved_as_https(self):
        for source in ("ssh://git@github.com/VELA-DESKTOP/Vela.git", "git@github.com:vela-desktop/vela.git"):
            with self.subTest(source=source):
                self.run_update("--source", source, "--check", source=False)
                self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
                self.assert_https_transport()

    def test_remembered_github_ssh_sources_are_migrated(self):
        for source in ("ssh://git@github.com/VELA-DESKTOP/Vela.git", "git@github.com:vela-desktop/vela.git"):
            with self.subTest(source=source):
                self.write_config(source)
                self.run_update("--check", source=False)
                self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
                self.assert_https_transport()

    def test_old_config_filename_is_migrated_to_https(self):
        self.config.parent.mkdir(parents=True)
        legacy = self.config.with_name("aggiornamenti.conf")
        legacy.write_text("sorgente=git@github.com:vela-desktop/vela.git\n")
        self.run_update("--check", source=False)
        self.assertFalse(legacy.exists())
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        self.assert_https_transport()

    def test_official_ssh_mirror_is_migrated_without_recloning(self):
        self.run_update("--check", source=False)
        for index, source in enumerate(("ssh://git@github.com/VELA-DESKTOP/Vela.git",
                                        "git@github.com:vela-desktop/vela.git"), start=1):
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

    def test_official_ssh_origin_uses_org_for_ci_artifacts(self):
        self.git("remote", "set-url", "origin", "git@github.com:vela-desktop/vela.git")
        self.env.update(TEST_CI_RUN="123", TEST_DOWNLOAD_SHA=self.main[:7])
        self.run_update("--download")
        listing = next(call for call in self.calls("gh")
                       if call["args"][:2] == ["run", "list"])
        self.assertEqual(listing["args"][listing["args"].index("-R") + 1],
                         "vela-desktop/vela")

    def test_local_copy_with_ssh_origin_stays_local(self):
        self.git("remote", "set-url", "origin", "git@github.com:vela-desktop/vela.git")
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


    def test_legacy_refspec_migrates_without_pruning_private_pin(self):
        self.run_update("--build", source=False)
        pin = self.mirror / "refs/vela-update/selected"
        self.assertEqual(pin.read_text().strip(), self.main)
        subprocess.run(["git", "-C", str(self.mirror), "config", "--replace-all",
                        "remote.origin.fetch", "+refs/*:refs/*"], env=self.env, check=True)
        self.run_update("--check", source=False)
        self.assertEqual(pin.read_text().strip(), self.main)
        specs = subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "--get-all", "remote.origin.fetch"],
            env=self.env, text=True).splitlines()
        self.assertEqual(specs, ["+refs/heads/*:refs/heads/*",
                                 "+refs/tags/*:refs/tags/*"])
        self.assertFalse(list(self.mirror.parent.glob("vela-corrupt.*")))

    CORRUPT_OID = "aa" + "b" * 38

    def corrupt_cached_mirror(self):
        oid = self.CORRUPT_OID
        obj = self.mirror / "objects/aa" / ("b" * 38)
        obj.parent.mkdir(parents=True, exist_ok=True)
        obj.touch()
        ref = self.mirror / "refs/vela-update/selected"
        ref.parent.mkdir(parents=True, exist_ok=True)
        ref.write_text(oid + "\n")
        return ref

    def test_corrupt_cache_reclones_and_preserves_old_mirror(self):
        self.run_update("--check", source=False)
        self.corrupt_cached_mirror()
        result = self.run_update("--check", source=False)
        self.assertIn("corrupt", result.stderr)
        # git fsck's own report reaches the user. Its wording depends on Git's
        # version and language; every version names the broken object.
        self.assertIn(self.CORRUPT_OID, result.stderr)
        self.assertIn("preserved at", result.stderr)
        self.assertFalse((self.mirror / "refs/vela-update/selected").exists())
        quarantine = list(self.mirror.parent.glob("vela-corrupt.*"))
        self.assertEqual(len(quarantine), 1)
        self.assertTrue((quarantine[0] / "refs/vela-update/selected").exists())
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_signal_before_old_mirror_rename_leaves_original_untouched(self):
        self.run_update("--check", source=False)
        self.corrupt_cached_mirror()
        self.env["TEST_INTERRUPT_OLD_MIRROR_SWAP"] = "1"
        try:
            result = self.run_update("--check", source=False, status=143)
        finally:
            self.env.pop("TEST_INTERRUPT_OLD_MIRROR_SWAP")
        self.assertIn("cached Git mirror is corrupt", result.stderr)
        self.assertTrue((self.mirror / "refs/vela-update/selected").exists())
        self.assertNotIn("previous mirror is preserved at", result.stderr)
        self.assertFalse(list(self.mirror.parent.glob("vela-corrupt.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))

    def test_signal_during_mirror_swap_restores_corrupt_cache(self):
        self.run_update("--check", source=False)
        self.corrupt_cached_mirror()
        self.env["TEST_INTERRUPT_MIRROR_SWAP"] = "1"
        try:
            result = self.run_update("--check", source=False, status=143)
        finally:
            self.env.pop("TEST_INTERRUPT_MIRROR_SWAP")
        self.assertIn("cached Git mirror is corrupt", result.stderr)
        self.assertTrue((self.mirror / "refs/vela-update/selected").exists())
        self.assertFalse(list(self.mirror.parent.glob("vela-corrupt.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_signal_after_source_config_rename_restores_both_old_paths(self):
        self.run_update("--check")
        original_config = self.config.read_bytes()
        original_origin = subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip()
        alternate = self.directory / "alternate"
        shutil.copytree(self.source, alternate)
        self.env["TEST_INTERRUPT_CONFIG_COMMIT"] = "1"
        try:
            result = self.run_update("--source", str(alternate), "--check",
                                     source=False, status=143)
        finally:
            self.env.pop("TEST_INTERRUPT_CONFIG_COMMIT")
        self.assertEqual(self.config.read_bytes(), original_config)
        self.assertEqual(subprocess.check_output(
            ["git", "-C", str(self.mirror), "config", "remote.origin.url"],
            env=self.env, text=True).strip(), original_origin)
        self.assertFalse(list(self.config.parent.glob(".updates.conf.backup.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-backup.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])
        self.run_update("--check", source=False)

    def test_signal_after_initial_config_rename_removes_uncommitted_paths(self):
        self.env["TEST_INTERRUPT_CONFIG_COMMIT"] = "1"
        try:
            self.run_update("--source", HTTPS_SOURCE, "--check",
                            source=False, status=143)
        finally:
            self.env.pop("TEST_INTERRUPT_CONFIG_COMMIT")
        self.assertFalse(self.config.exists())
        self.assertFalse(self.mirror.exists())
        self.assertFalse(list(self.config.parent.glob(".updates.conf.backup.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_signal_during_config_commit_restores_corrupt_cache(self):
        self.run_update("--check", source=False)
        self.corrupt_cached_mirror()
        self.write_config(HTTPS_SOURCE)
        self.env["TEST_INTERRUPT_CONFIG_COMMIT"] = "1"
        try:
            result = self.run_update("--source", HTTPS_SOURCE, "--check",
                                     source=False, status=143)
        finally:
            self.env.pop("TEST_INTERRUPT_CONFIG_COMMIT")
        self.assertIn("cached Git mirror is corrupt", result.stderr)
        self.assertTrue((self.mirror / "refs/vela-update/selected").exists())
        self.assertEqual(self.config.read_text(), "source=" + HTTPS_SOURCE + "\n")
        self.assertFalse(list(self.mirror.parent.glob("vela-corrupt.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_failed_corrupt_cache_reclone_keeps_original(self):
        self.run_update("--check", source=False)
        self.corrupt_cached_mirror()
        self.env["TEST_FAIL_FETCH"] = "1"
        result = self.run_update("--check", source=False, status=1)
        self.assertIn("simulated Git transport failure", result.stderr)
        self.assertTrue((self.mirror / "refs/vela-update/selected").exists())
        self.assertFalse(list(self.mirror.parent.glob("vela-corrupt.*")))
        self.assertFalse(list(self.mirror.parent.glob("vela-mirror.*")))

    def test_transient_fetch_failure_reports_git_error_and_does_not_reclone(self):
        self.run_update("--check", source=False)
        self.env["TEST_FAIL_FETCH"] = "1"
        result = self.run_update("--check", source=False, status=1)
        self.assertIn("simulated Git transport failure", result.stderr)
        self.assertNotIn("cached Git mirror is corrupt", result.stderr)
        clones = [c for c in self.calls("git") if c["operation"] == "clone"]
        self.assertEqual(len(clones), 1)


class UpdatePublicReleases(UpdateFixture):
    def test_public_package_installs_without_gh_login_or_build(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_REQUIRE_MAIN_REF="1")
        self.run_update("--download", source=False)
        self.assertEqual(len(self.calls("curl")), 4)
        self.assertTrue(any("/alpha-" + self.main + "/" in c["args"][-1]
                            for c in self.calls("curl")))
        verification = next(call for call in self.calls("cosign")
                            if call["args"][:1] == ["verify-blob-attestation"])
        self.assertEqual(verification["args"][verification["args"].index("--certificate-github-workflow-sha") + 1],
                         self.main)
        self.assertEqual(self.calls("makepkg"), [])
        self.assertEqual(len(self.calls("sudo")), 1)
        self.assertIn(".g" + self.main[:7] + "-", self.calls("sudo")[0]["args"][-1])

    def test_check_only_fetches_manifest(self):
        self.env["TEST_RELEASE_SHA"] = self.main
        result = self.run_update("--check", source=False)
        self.assertIn("Public alpha package found", result.stdout)
        self.assertEqual(len(self.calls("curl")), 1)
        self.assertEqual(self.calls("makepkg") + self.calls("sudo"), [])

    def test_force_local_build_skips_public_release(self):
        self.env["TEST_RELEASE_SHA"] = self.main
        self.run_update("--build", source=False)
        self.assertEqual(self.calls("curl"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_wrong_release_commit_is_not_downloaded(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_RELEASE_PKG_SHA=self.nvidia)
        result = self.run_update("--download", source=False)
        self.assertIn("invalid public release manifest", result.stderr)
        self.assertEqual(len(self.calls("curl")), 1)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_bad_digest_falls_back_to_local_build(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_BAD_RELEASE_DIGEST="1")
        result = self.run_update("--download", source=False)
        self.assertIn("unverified or corrupt", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_corrupted_download_falls_back_to_local_build(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_CORRUPT_RELEASE="1")
        self.run_update("--download", source=False)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_official_url_alias_release_uses_canonical_provenance(self):
        self.write_config("https://github.com/vela-desktop/vela")
        self.env["TEST_RELEASE_SHA"] = self.main
        self.run_update("--download", source=False)
        self.assertEqual(self.config.read_text(), f"source={HTTPS_SOURCE}\n")
        verification = next(call for call in self.calls("cosign")
                            if call["args"][:1] == ["verify-blob-attestation"])
        args = verification["args"]
        self.assertEqual(args[args.index("--certificate-github-workflow-repository") + 1], "vela-desktop/vela")
        self.assertIn("--certificate-identity", args)
        self.assertEqual(self.calls("makepkg"), [])
        self.assertEqual(len(self.calls("sudo")), 1)

    def test_public_release_attestation_failure_shows_reason_and_builds(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_REJECT_ATTEST="1")
        result = self.run_update("--download")
        self.assertIn("simulated cosign attestation rejection", result.stderr)
        self.assertIn("provenance verification failed", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_public_release_without_attestation_falls_back_to_source(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_REJECT_ATTEST="1")
        result = self.run_update("--download", source=False)
        self.assertIn("provenance verification failed", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)
        self.assertFalse(list((self.mirror.parent / "packages").glob("*.pkg.tar.zst")))

    def test_signed_self_hosted_runner_is_rejected(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_SELF_HOSTED="1")
        result = self.run_update("--download")
        self.assertIn("non-GitHub-hosted runner", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_public_release_with_wrong_source_provenance_falls_back(self):
        self.env.update(TEST_RELEASE_SHA=self.main, TEST_WRONG_ATTEST_SHA="1")
        self.run_update("--download", source=False)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_missing_cosign_never_installs_unattested_binary(self):
        self.env["TEST_RELEASE_SHA"] = self.main
        # Make the verification executable unusable; a missing verifier must
        # fall back to local sources even if GitHub serves a valid checksum.
        cosign = self.directory / "bin/cosign"
        cosign.write_text("not an executable")
        result = self.run_update("--download", source=False)
        self.assertIn("provenance verification failed", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_absent_release_keeps_build_fallback(self):
        result = self.run_update("--download", source=False)
        self.assertNotIn("release lookup failed", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_invalid_manifest_reports_error_and_builds(self):
        for mode in ("invalid-digest", "empty", "multiple"):
            with self.subTest(mode=mode):
                self.env.update(TEST_RELEASE_SHA=self.main, TEST_BAD_RELEASE_MANIFEST=mode)
                result = self.run_update("--download", source=False)
                self.assertIn("invalid public release manifest", result.stderr)
                self.assertEqual(self.calls("sudo"), [])
                self.assertEqual(self.calls("makepkg")[-1]["sha"], self.main)

    def test_http_failures_other_than_404_are_reported_and_build(self):
        for code in ("403", "429", "500", "503"):
            with self.subTest(http=code):
                self.env["TEST_RELEASE_HTTP"] = code
                result = self.run_update("--download", source=False)
                self.assertIn("HTTP " + code, result.stderr)
                self.assertIn("optional public release lookup failed", result.stderr)
                self.assertEqual(self.calls("sudo"), [])
                self.assertEqual(self.calls("makepkg")[-1]["sha"], self.main)

    def test_http_404_is_quiet_and_falls_back_to_build(self):
        self.env["TEST_RELEASE_HTTP"] = "404"
        result = self.run_update("--download", source=False)
        self.assertNotIn("optional public release lookup failed", result.stderr)
        self.assertEqual(self.calls("sudo"), [])
        self.assertEqual(self.calls("makepkg")[0]["sha"], self.main)

    def test_release_lookup_network_error_is_visible_without_aborting_build(self):
        self.env["TEST_RELEASE_TIMEOUT"] = "1"
        result = self.run_update("--download", source=False)
        self.assertIn("release lookup failed", result.stderr)
        self.assertIn("simulated release lookup timeout", result.stderr)
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

    def test_interactive_build_preserves_tty_and_stops_descendants(self):
        # Simulate launching vela-update from a real terminal, with sudo
        # opening /dev/tty and a compiler grandchild running underneath makepkg.
        self.env["TEST_REQUIRE_TTY"] = "1"
        self.env["TEST_SPAWN_BUILD_CHILD"] = "1"
        ready = self.directory / "interactive-child-ready"
        signalled = self.directory / "interactive-child-signalled"
        self.env["TEST_BUILD_CHILD_READY"] = str(ready)
        self.env["TEST_BUILD_CHILD_SIGNALLED"] = str(signalled)
        proc, release = self.start_paused_build(interactive=True)
        self.assertTrue(ready.exists())
        try:
            proc.send_signal(signal.SIGTERM)
            stdout, stderr = proc.communicate(timeout=8)
        finally:
            release.touch()
        self.assertEqual(proc.returncode, 143, stdout + stderr)
        self.assertTrue(signalled.exists(),
                        "The interactive build's compiler child did not stop")
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
