#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Maintainer commands for Beta releases (docs/release-automation.md).

`rehearse 0.2.1` checks the candidate commit and pushes the
rehearsal/v0.2.1 tag, which runs the whole Beta pipeline and publishes a
draft release; an earlier rehearsal of the same version is replaced.
`clean 0.2.1` deletes that draft and its tag.

The candidate is the tip of release/0.<minor> for a patch release and of
main for a .0 release, unless --commit names a commit on that branch. Its
project(VERSION ...) must be the version, and v<version> must not exist.

Needs git with push access to the remote and an authenticated gh.
"""

import argparse
import json
import re
import subprocess
import sys

VERSION = re.compile(r"0\.(\d+)\.(\d+)")
PROJECT_VERSION = re.compile(r"^\s*VERSION\s+([0-9][0-9.]*)", re.M)


class ReleaseError(Exception):
    pass


def run(*cmd, check=True):
    result = subprocess.run(cmd, capture_output=True, text=True)
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


def candidate(version, remote, commit=None):
    branch = branch_for(version)
    run("git", "fetch", "--quiet", remote, f"+refs/heads/{branch}:refs/remotes/{remote}/{branch}")
    tip = run("git", "rev-parse", "--verify", f"refs/remotes/{remote}/{branch}^{{commit}}").stdout.strip()
    sha = tip
    if commit:
        sha = run("git", "rev-parse", "--verify", f"{commit}^{{commit}}").stdout.strip()
        if run("git", "merge-base", "--is-ancestor", sha, tip, check=False).returncode != 0:
            raise ReleaseError(f"{sha[:12]} is not on {remote}/{branch}, where v{version} is tagged")
    cmake = run("git", "show", f"{sha}:CMakeLists.txt").stdout
    found = PROJECT_VERSION.search(cmake)
    if not found or found[1] != version:
        raise ReleaseError(f"{sha[:12]} has project(VERSION {found[1] if found else '?'}), not {version}: "
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


def rehearse(version, remote, repo, commit=None):
    sha = candidate(version, remote, commit)
    clean(version, remote, repo)
    tag = f"rehearsal/v{version}"
    run("git", "push", "--quiet", remote, f"{sha}:refs/tags/{tag}")
    print(f"Pushed {tag} on {sha}")
    print(f"Pipeline: https://github.com/{repo}/actions/workflows/build.yml?query=branch%3A{tag.replace('/', '%2F')}")
    print(f"Remove it with: scripts/release.py clean {version}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--remote", default="origin", help="git remote of the repository (default: origin)")
    parser.add_argument("--repo", default="vela-desktop/vela", help="GitHub repository (default: vela-desktop/vela)")
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("rehearse", help="push rehearsal/v<version> on the candidate commit")
    p.add_argument("version")
    p.add_argument("--commit", help="candidate commit (default: the branch tip)")
    p = commands.add_parser("clean", help="delete the rehearsal draft release and tag")
    p.add_argument("version")
    args = parser.parse_args(argv)
    try:
        if args.command == "rehearse":
            rehearse(args.version, args.remote, args.repo, args.commit)
        else:
            clean(args.version, args.remote, args.repo)
    except ReleaseError as error:
        print(f"release.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
