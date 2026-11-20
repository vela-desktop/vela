#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Maintainer commands for Beta releases (docs/release-automation.md).

  prepare 0.2.1   open the pull request that sets project(VERSION 0.2.1)
                  and turns "Unreleased" into the 0.2.1 CHANGELOG section
  rehearse 0.2.1  check the candidate commit, push rehearsal/v0.2.1 (the
                  whole Beta pipeline, publishing a draft) and start both
                  GPU suites on it; an earlier rehearsal is replaced
  status 0.2.1    whether the rehearsed commit can be tagged
  tag 0.2.1       sign v0.2.1 on the rehearsed commit with your key, push
                  it and remove the rehearsal
  clean 0.2.1     delete the rehearsal's draft and tag
  finish 0.2.1    once v0.2.1 is published: open the pull requests that move
                  manual builds (PKGBUILD.beta) to it; after a .0, create
                  release/0.<minor> from the tag and move main to the next
                  minor version

The candidate is the tip of release/0.<minor> for a patch release and of
main for a .0 release, unless --commit names a commit on that branch. Its
project(VERSION ...) must be the version, and v<version> must not exist.

Needs git with push access to the remote, a signing key configured for git
tags, and an authenticated gh.
"""

import argparse
import datetime
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

VERSION = re.compile(r"0\.(\d+)\.(\d+)")
PROJECT_VERSION = re.compile(r"^(\s*VERSION\s+)([0-9][0-9.]*)", re.M)
UNRELEASED = "## Unreleased\n"
MONTHS = ("January", "February", "March", "April", "May", "June", "July",
          "August", "September", "October", "November", "December")
# Manual Arch builds fetch this tag when VELA_RELEASE_VERSION is unset.
FALLBACK = re.compile(r"^(pkgver=\$\{VELA_RELEASE_VERSION:-)([0-9][0-9.]*)(\})", re.M)
PKGBUILD_BETA = "packaging/arch/PKGBUILD.beta"
MANIFESTS = ("SHA256SUMS", "SHA256SUMS-fedora", "SHA256SUMS-fedora-45", "SHA256SUMS-ubuntu")
# A full suite keeps the workflow's plain name (run-name in each workflow).
GPU_SUITES = {"gpu-amd.yml": "GPU CI - AMD (manual)", "gpu-nvidia.yml": "GPU CI - NVIDIA (manual)"}


class ReleaseError(Exception):
    pass


def run(*cmd, check=True, cwd=None):
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if check and result.returncode != 0:
        raise ReleaseError(f"{' '.join(cmd)} failed: {result.stderr.strip() or result.stdout.strip()}")
    return result


def parse_version(text):
    match = VERSION.fullmatch(text)
    if not match:
        raise ReleaseError(f"not a Beta version (0.<minor>.<patch>): {text}")
    return int(match[1]), int(match[2])


def branch_for(version):
    minor, patch = parse_version(version)
    return f"release/0.{minor}" if patch else "main"


def remote_tag(remote, tag):
    """The commit a remote tag points to, or None."""
    out = run("git", "ls-remote", remote, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}").stdout
    refs = dict(reversed(line.split("\t", 1)) for line in out.splitlines() if "\t" in line)
    return refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")


def fetch_branch(version, remote):
    branch = branch_for(version)
    run("git", "fetch", "--quiet", remote, f"+refs/heads/{branch}:refs/remotes/{remote}/{branch}")
    tip = run("git", "rev-parse", "--verify", f"refs/remotes/{remote}/{branch}^{{commit}}").stdout.strip()
    return branch, tip


def project_version(sha):
    found = PROJECT_VERSION.search(run("git", "show", f"{sha}:CMakeLists.txt").stdout)
    return found[2] if found else None


def candidate(version, remote, commit=None):
    branch, tip = fetch_branch(version, remote)
    sha = tip
    if commit:
        sha = run("git", "rev-parse", "--verify", f"{commit}^{{commit}}").stdout.strip()
        if run("git", "merge-base", "--is-ancestor", sha, tip, check=False).returncode != 0:
            raise ReleaseError(f"{sha[:12]} is not on {remote}/{branch}, where v{version} is tagged")
    found = project_version(sha)
    if found != version:
        raise ReleaseError(f"{sha[:12]} has project(VERSION {found or '?'}), not {version}: "
                           f"merge the version bump on {branch} first")
    if remote_tag(remote, f"v{version}"):
        raise ReleaseError(f"v{version} is already tagged: a published version can't be rehearsed again")
    return sha


def release_state(repo, tag):
    """'draft', 'published' or None for the release of a tag."""
    result = run("gh", "release", "view", tag, "--repo", repo, "--json", "isDraft", check=False)
    if result.returncode != 0:
        return None
    return "draft" if json.loads(result.stdout)["isDraft"] else "published"


# --- prepare ---------------------------------------------------------------

def changelog_date(day):
    return f"{day.day} {MONTHS[day.month - 1]} {day.year}"


def release_changelog(text, version, day):
    """Move the Unreleased entries under a new section for version."""
    if re.search(rf"(?m)^## {re.escape(version)}\b", text):
        raise ReleaseError(f"CHANGELOG.md already has a {version} section")
    head, sep, rest = text.partition(UNRELEASED)
    if not sep:
        raise ReleaseError("CHANGELOG.md has no \"## Unreleased\" section")
    body, nxt, tail = rest.partition("\n## ")
    if not body.strip():
        raise ReleaseError("nothing under \"Unreleased\" in CHANGELOG.md: no user-visible change to release")
    section = f"## {version} — {changelog_date(day)}\n\n{body.strip()}\n"
    return f"{head}{UNRELEASED}\n{section}" + (f"\n## {tail}" if nxt else "")


def bump_project_version(text, version):
    found = PROJECT_VERSION.search(text)
    if not found:
        raise ReleaseError("no project(VERSION ...) in CMakeLists.txt")
    return text[:found.start(2)] + version + text[found.end(2):]


def prepare(version, remote, repo, day=None):
    day = day or datetime.datetime.now(datetime.UTC).date()
    branch, tip = fetch_branch(version, remote)
    if remote_tag(remote, f"v{version}"):
        raise ReleaseError(f"v{version} is already tagged")
    current = project_version(tip)
    _, patch = parse_version(version)
    if patch and current and tuple(map(int, current.split("."))) >= tuple(map(int, version.split("."))):
        raise ReleaseError(f"{branch} is already at {current}")
    if not patch and current != version:
        raise ReleaseError(f"main is at {current}, not {version}: a .0 release is tagged on main as it is")
    head = f"prepare/v{version}"
    with tempfile.TemporaryDirectory(prefix="vela-prepare-") as directory:
        tree = Path(directory) / "tree"
        run("git", "worktree", "add", "--quiet", "--detach", str(tree), tip)
        try:
            cmake = tree / "CMakeLists.txt"
            cmake.write_text(bump_project_version(cmake.read_text(), version))
            changelog = tree / "CHANGELOG.md"
            changelog.write_text(release_changelog(changelog.read_text(), version, day))
            run("git", "add", "CMakeLists.txt", "CHANGELOG.md", cwd=tree)
            run("git", "commit", "--quiet", "-s", "-m", f"release: prepare version {version}", cwd=tree)
            run("git", "push", "--quiet", remote, f"HEAD:refs/heads/{head}", cwd=tree)
        finally:
            run("git", "worktree", "remove", "--force", str(tree), check=False)
    body = (f"Sets `project(VERSION {version})` and moves the \"Unreleased\" CHANGELOG entries "
            f"under {version}. After it merges: `scripts/release.py rehearse {version}`.")
    url = run("gh", "pr", "create", "--repo", repo, "--base", branch, "--head", head,
              "--title", f"release: prepare version {version}", "--body", body).stdout.strip()
    print(f"Opened {url}")


# --- rehearse, status, tag, clean -------------------------------------------

def clean(version, remote, repo):
    parse_version(version)
    tag = f"rehearsal/v{version}"
    state = release_state(repo, tag)
    if state == "published":
        raise ReleaseError(f"the release of {tag} is published, not a draft: check it by hand")
    if state == "draft":
        run("gh", "release", "delete", tag, "--repo", repo, "--yes")
        print(f"Deleted the draft release of {tag}")
    if remote_tag(remote, tag):
        run("git", "push", "--quiet", remote, f":refs/tags/{tag}")
        print(f"Deleted the tag {tag}")


def rehearse(version, remote, repo, commit=None, gpu=True):
    sha = candidate(version, remote, commit)
    clean(version, remote, repo)
    tag = f"rehearsal/v{version}"
    run("git", "push", "--quiet", remote, f"{sha}:refs/tags/{tag}")
    print(f"Pushed {tag} on {sha}")
    print(f"Pipeline: https://github.com/{repo}/actions/workflows/build.yml?query=branch%3A{tag.replace('/', '%2F')}")
    if gpu:
        for workflow in GPU_SUITES:
            run("gh", "workflow", "run", workflow, "--repo", repo, "--ref", tag)
            print(f"Started {workflow} on {tag}")
    print(f"Then: scripts/release.py status {version}; remove it with: scripts/release.py clean {version}")


def latest_run(repo, workflow, sha, branch=None, title=None):
    args = ["gh", "run", "list", "--repo", repo, "--workflow", workflow, "--limit", "100",
            "--json", "headSha,displayTitle,status,conclusion,url"]
    if branch:
        args += ["--branch", branch]
    runs = [r for r in json.loads(run(*args).stdout)
            if r["headSha"] == sha and (title is None or r["displayTitle"] == title)]
    # A success on the exact commit counts even if a later retry is running.
    return next((r for r in runs if r["conclusion"] == "success"), runs[0] if runs else None)


def describe(found):
    if found is None:
        return False, "no run on this commit"
    if found["status"] != "completed":
        return False, f"{found['status']} {found['url']}"
    return found["conclusion"] == "success", f"{found['conclusion']} {found['url']}"


def status(version, remote, repo, quiet=False):
    """(ok, commit): whether the rehearsed commit is ready to be tagged."""
    parse_version(version)
    tag = f"rehearsal/v{version}"
    sha = remote_tag(remote, tag)
    if not sha:
        raise ReleaseError(f"no {tag}: run scripts/release.py rehearse {version} first")
    checks = []
    try:
        candidate(version, remote, sha)
        checks.append(("candidate", True, f"{sha[:12]} on {branch_for(version)}, project(VERSION {version})"))
    except ReleaseError as error:
        checks.append(("candidate", False, str(error)))
    checks.append(("pipeline",) + describe(latest_run(repo, "build.yml", sha, branch=tag)))
    state = release_state(repo, tag)
    checks.append(("draft release", state == "draft", state or "missing"))
    for workflow, title in GPU_SUITES.items():
        checks.append((title,) + describe(latest_run(repo, workflow, sha, title=title)))
    ok = all(passed for _, passed, _ in checks)
    if not quiet:
        for name, passed, detail in checks:
            print(f"{'ok  ' if passed else 'FAIL'} {name}: {detail}")
        print(f"v{version} {'can' if ok else 'cannot'} be tagged on {sha[:12]}")
    return ok, sha


def tag(version, remote, repo, yes=False, ask=input):
    ok, sha = status(version, remote, repo)
    if not ok:
        raise ReleaseError("the rehearsal is not green: nothing tagged")
    name = f"v{version}"
    if not yes and ask(f"Type {name} to sign and push it on {sha[:12]} (it can't be undone): ").strip() != name:
        raise ReleaseError("not confirmed: nothing tagged")
    local = run("git", "rev-parse", "--verify", "--quiet", f"refs/tags/{name}^{{commit}}", check=False).stdout.strip()
    if local and local != sha:
        raise ReleaseError(f"a local {name} points to {local[:12]}, not {sha[:12]}: delete it first")
    if not local:
        run("git", "tag", "-s", "-m", f"Vela {version} Beta", name, sha)
    if "-----BEGIN " not in run("git", "cat-file", "tag", name).stdout:
        raise ReleaseError(f"{name} is not signed: configure a signing key for git tags")
    run("git", "push", "--quiet", remote, f"refs/tags/{name}")
    print(f"Pushed the signed tag {name} on {sha}")
    print(f"Pipeline: https://github.com/{repo}/actions/workflows/build.yml?query=branch%3A{name}")
    clean(version, remote, repo)
    print(f"Once it is published: scripts/release.py finish {version}")


# --- finish ------------------------------------------------------------------

def remote_branch(remote, branch):
    out = run("git", "ls-remote", remote, f"refs/heads/{branch}").stdout.split()
    return out[0] if out else None


def open_change(remote, repo, base, head, title, body, edit):
    """Open or reuse the pull request for an edit on top of the current base."""
    base_ref = f"refs/remotes/{remote}/{base}"
    run("git", "fetch", "--quiet", remote, f"+refs/heads/{base}:{base_ref}")
    with tempfile.TemporaryDirectory(prefix="vela-finish-") as directory:
        tree = Path(directory) / "tree"
        run("git", "worktree", "add", "--quiet", "--detach", str(tree), base_ref)
        try:
            edit(tree)
            if not run("git", "status", "--porcelain", cwd=tree).stdout.strip():
                print(f"{base} needs no change")
                return
            existing = remote_branch(remote, head)
            if existing:
                run("git", "fetch", "--quiet", remote, f"refs/heads/{head}")
                # Compare the proposed merge with the intended edit on today's base.
                run("git", "add", "-u", cwd=tree)
                expected = run("git", "write-tree", cwd=tree).stdout.strip()
                merged = run("git", "merge-tree", "--write-tree", "HEAD", existing, cwd=tree, check=False)
                if merged.returncode != 0 or merged.stdout.strip() != expected:
                    raise ReleaseError(f"{remote}/{head} does not contain the intended change for {base}: review it before retrying")
            else:
                run("git", "commit", "--quiet", "-s", "-a", "-m", title, cwd=tree)
                run("git", "push", "--quiet", remote, f"HEAD:refs/heads/{head}", cwd=tree)
        finally:
            run("git", "worktree", "remove", "--force", str(tree), check=False)
    if existing:
        result = run("gh", "pr", "list", "--repo", repo, "--base", base, "--head", head,
                     "--state", "open", "--json", "url,isCrossRepository")
        for pr in json.loads(result.stdout):
            if not pr["isCrossRepository"]:
                print(f"Already open: {pr['url']}")
                return
    url = run("gh", "pr", "create", "--repo", repo, "--base", base, "--head", head,
              "--title", title, "--body", body).stdout.strip()
    print(f"Opened {url}")


def set_fallback(tree, version):
    path = tree / PKGBUILD_BETA
    text = path.read_text()
    match = FALLBACK.search(text)
    if not match:
        raise ReleaseError(f"no pkgver fallback in {PKGBUILD_BETA}")
    if parse_version(match[2]) < parse_version(version):
        path.write_text(text[:match.start(2)] + version + text[match.end(2):])


def finish(version, remote, repo):
    minor, patch = parse_version(version)
    name = f"v{version}"
    view = run("gh", "release", "view", name, "--repo", repo, "--json", "isDraft,assets", check=False)
    release = json.loads(view.stdout) if view.returncode == 0 else None
    if not release or release["isDraft"]:
        raise ReleaseError(f"{name} is not published yet: wait for its pipeline")
    missing = set(MANIFESTS) - {asset["name"] for asset in release["assets"]}
    if missing:
        raise ReleaseError(f"{name} lacks {', '.join(sorted(missing))}: check its pipeline")
    sha = remote_tag(remote, name)
    if not sha:
        raise ReleaseError(f"{name} is published but its tag is missing")
    branch = f"release/0.{minor}"
    if not remote_branch(remote, branch):
        if patch:
            raise ReleaseError(f"{branch} does not exist, but {name} is a patch release")
        result = run("git", "push", "--quiet", remote, f"{sha}:refs/heads/{branch}", check=False)
        if result.returncode != 0:
            raise ReleaseError(f"could not create {branch} from {name} ({result.stderr.strip()}); "
                               f"create it from the tag on GitHub, then run finish again")
        print(f"Created {branch} from {name}")
    body = (f"{name} is published. Advance the fallback for manual Beta builds in "
            f"`{PKGBUILD_BETA}` if it still points to an older release.")
    open_change(remote, repo, branch, f"finish/{name}-release-0.{minor}",
                f"packaging: manual Beta builds fetch {name}", body,
                lambda tree: set_fallback(tree, version))
    following = f"0.{minor + 1}.0"

    def main_edit(tree):
        set_fallback(tree, version)
        cmake = tree / "CMakeLists.txt"
        found = PROJECT_VERSION.search(cmake.read_text())
        # Only the first finish after a .0 moves main on; a rerun finds it moved.
        if not patch and found and found[2] == version:
            cmake.write_text(bump_project_version(cmake.read_text(), following))

    title = (f"release: start {following}; manual Beta builds fetch {name}" if not patch
             else f"packaging: manual Beta builds fetch {name}")
    main_body = body + (f" {branch} now carries 0.{minor}.x, and main moves to {following}." if not patch else "")
    open_change(remote, repo, "main", f"finish/{name}-main", title, main_body, main_edit)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--remote", default="origin", help="git remote of the repository (default: origin)")
    parser.add_argument("--repo", default="vela-desktop/vela", help="GitHub repository (default: vela-desktop/vela)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("prepare", help="open the version bump pull request").add_argument("version")
    p = commands.add_parser("rehearse", help="push rehearsal/v<version> and start the GPU suites")
    p.add_argument("version")
    p.add_argument("--commit", help="candidate commit (default: the branch tip)")
    p.add_argument("--no-gpu", action="store_true", help="don't start the GPU suites")
    commands.add_parser("status", help="whether the rehearsal can be tagged").add_argument("version")
    p = commands.add_parser("tag", help="sign and push v<version> on the rehearsed commit")
    p.add_argument("version")
    p.add_argument("--yes", action="store_true", help="don't ask for confirmation")
    commands.add_parser("clean", help="delete the rehearsal draft release and tag").add_argument("version")
    commands.add_parser("finish", help="pull requests after v<version> is published").add_argument("version")
    args = parser.parse_args(argv)
    try:
        parse_version(args.version)
        if args.command == "prepare":
            prepare(args.version, args.remote, args.repo)
        elif args.command == "rehearse":
            rehearse(args.version, args.remote, args.repo, args.commit, gpu=not args.no_gpu)
        elif args.command == "status":
            return 0 if status(args.version, args.remote, args.repo)[0] else 1
        elif args.command == "tag":
            tag(args.version, args.remote, args.repo, args.yes)
        elif args.command == "finish":
            finish(args.version, args.remote, args.repo)
        else:
            clean(args.version, args.remote, args.repo)
    except ReleaseError as error:
        print(f"release.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
