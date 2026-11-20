#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Upgrade from the previous published Beta in a clean container (CI only).

`packages --version 0.2.1 --candidate DIR` installs the newest published
Beta older than 0.2.1, then the candidate package in DIR with the native
package manager, and checks the result. It runs before publication, on a
release tag and on its rehearsal (docs/release-automation.md §5, R2).

`updater --version 0.2.1 --checkout DIR` installs the previous Beta and
lets its own updater install the published 0.2.1, then checks that the new
updater finds nothing newer. Updaters before 0.2.0 only see prereleases, so
from those the documented one-time step runs `packaging/vela-update` from
DIR, a checkout of the release (release-channels.md, "Updating older Beta
installations").

The distribution comes from /etc/os-release. Runs as root.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import urllib.request

REPO = "vela-desktop/vela"
API = f"https://api.github.com/repos/{REPO}/releases"
DOWNLOAD = f"https://github.com/{REPO}/releases/download"
TAG = re.compile(r"v(0\.\d+\.\d+)")
# The first updater that discovers Betas published as normal releases (#282).
NORMAL_RELEASE_UPDATER = (0, 2, 0)
# Fedora ships no cosign: the upstream RPM, pinned (installation.md).
COSIGN_RPM = ("https://github.com/sigstore/cosign/releases/download/v3.1.4/cosign-3.1.4-1.x86_64.rpm",
              "02fd4fe524932ccba09f7762fce76be35e5570d2023a1093cce776eefc6b62df")


class UpgradeError(Exception):
    pass


def version_key(text):
    return tuple(map(int, text.split(".")))


def os_release(path="/etc/os-release"):
    fields = {}
    for line in Path(path).read_text().splitlines():
        key, sep, value = line.partition("=")
        if sep:
            fields[key] = value.strip().strip('"')
    return fields


def distro(fields):
    """arch, fedora-44, fedora-45 or ubuntu (26.10), as the updater supports."""
    ident, version = fields.get("ID", ""), fields.get("VERSION_ID", "")
    if ident in ("arch", "cachyos", "endeavouros"):
        return "arch"
    if ident == "fedora" and version in ("44", "45"):
        return f"fedora-{version}"
    if ident == "ubuntu" and version == "26.10":
        return "ubuntu"
    raise UpgradeError(f"unsupported distribution: {ident} {version}")


# The manifest that lists each distribution's package in a release.
MANIFEST = {"arch": "SHA256SUMS", "fedora-44": "SHA256SUMS-fedora",
            "fedora-45": "SHA256SUMS-fedora-45", "ubuntu": "SHA256SUMS-ubuntu"}


def candidate_pattern(name, version):
    v = re.escape(version)
    return re.compile({"arch": rf"vela-{v}-\d+-x86_64\.pkg\.tar\.zst",
                       "fedora-44": rf"vela-{v}-\d+\.fc44\.x86_64\.rpm",
                       "fedora-45": rf"vela-{v}-\d+\.fc45\.x86_64\.rpm",
                       "ubuntu": rf"vela_{v}_amd64\.deb"}[name])


def fetch(url, token=None):
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                                   "User-Agent": "vela-ci-beta-upgrade"})
    if token and url.startswith("https://api.github.com/"):
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def published_releases(token=None):
    releases = []
    for page in range(1, 11):
        batch = json.loads(fetch(f"{API}?per_page=100&page={page}", token))
        releases += batch
        if len(batch) < 100:
            break
    return releases


def previous_beta(version, releases, name):
    """The newest published Beta older than version that has this distro's
    manifest, or None. Drafts and other tags never count."""
    found = []
    for release in releases:
        match = TAG.fullmatch(release.get("tag_name", ""))
        if not match or release.get("draft"):
            continue
        assets = {asset.get("name") for asset in release.get("assets", [])}
        if version_key(match[1]) < version_key(version) and MANIFEST[name] in assets:
            found.append(match[1])
    return max(found, key=version_key, default=None)


def download_previous(version, name, directory):
    """Download the previous Beta's package for this distro and check it
    against its manifest."""
    manifest = fetch(f"{DOWNLOAD}/v{version}/{MANIFEST[name]}").decode()
    entries = [line.split() for line in manifest.splitlines() if line.strip()]
    if len(entries) != 1 or len(entries[0]) != 2:
        raise UpgradeError(f"v{version}/{MANIFEST[name]} should list exactly one package")
    digest, package = entries[0]
    data = fetch(f"{DOWNLOAD}/v{version}/{package}")
    if hashlib.sha256(data).hexdigest() != digest:
        raise UpgradeError(f"v{version}/{package} doesn't match its manifest")
    path = Path(directory) / package
    path.write_bytes(data)
    return path


def run(*cmd, stdin=None):
    print("+ " + " ".join(map(str, cmd)), flush=True)
    result = subprocess.run(list(map(str, cmd)), input=stdin, text=True)
    if result.returncode != 0:
        raise UpgradeError(f"{cmd[0]} failed with {result.returncode}")


def output(*cmd):
    return subprocess.run(list(map(str, cmd)), capture_output=True, text=True).stdout.strip()


def install(name, path):
    path = Path(path).resolve()
    if name == "arch":
        # vela-git would need --ask=4; a Beta-to-Beta upgrade must not.
        run("pacman", "-U", "--noconfirm", path)
    elif name.startswith("fedora"):
        run("dnf", "install", "-y", path)
    else:
        run("apt-get", "install", "-y", "--no-install-recommends", path)


def installed_version(name):
    if name == "arch":
        out = output("pacman", "-Q", "vela")  # "vela 0.2.1-1"
        return out.split()[1].rsplit("-", 1)[0] if out.startswith("vela ") else ""
    if name.startswith("fedora"):
        return output("rpm", "-q", "--qf", "%{VERSION}", "vela") if not subprocess.run(
            ["rpm", "-q", "vela"], capture_output=True).returncode else ""
    return output("dpkg-query", "-W", "-f=${Version}", "vela")


def check_installed(name, version):
    found = installed_version(name)
    if found != version:
        raise UpgradeError(f"vela {found or 'missing'} is installed, expected {version}")
    if name == "arch":
        run("pacman", "-Qkk", "vela")
    elif name.startswith("fedora"):
        run("rpm", "-V", "vela")
    else:
        run("dpkg", "--verify", "vela")
    run("vela-update", "--channel", "beta", "--help")
    print(f":: vela {version} is installed and its files match the package", flush=True)


def install_previous(version, name, releases, directory):
    previous = previous_beta(version, releases, name)
    if previous is None:
        print(f"::notice::No published Beta older than {version} for {name}: nothing to upgrade from")
        return None
    install(name, download_previous(previous, name, directory))
    check_installed(name, previous)
    return previous


def upgrade_packages(version, candidates, releases):
    name = distro(os_release())
    pattern = candidate_pattern(name, version)
    found = [path for path in Path(candidates).iterdir() if pattern.fullmatch(path.name)]
    if len(found) != 1:
        raise UpgradeError(f"expected one {name} package for {version} in {candidates}, found {len(found)}")
    with tempfile.TemporaryDirectory(prefix="vela-previous-") as directory:
        previous = install_previous(version, name, releases, directory)
        if previous is None:
            return
        install(name, found[0])
    check_installed(name, version)
    print(f":: {name}: upgraded from {previous} to {version} with the native package manager")


def install_cosign(name):
    if name == "arch":
        run("pacman", "-S", "--noconfirm", "--needed", "cosign")
    elif name == "ubuntu":
        run("apt-get", "install", "-y", "--no-install-recommends", "cosign")
    else:
        url, digest = COSIGN_RPM
        data = fetch(url)
        if hashlib.sha256(data).hexdigest() != digest:
            raise UpgradeError("the cosign RPM doesn't match its pinned checksum")
        with tempfile.TemporaryDirectory(prefix="vela-cosign-") as directory:
            path = Path(directory) / url.rsplit("/", 1)[1]
            path.write_bytes(data)
            run("dnf", "install", "-y", path)


def updater_command(previous, checkout):
    if version_key(previous) < NORMAL_RELEASE_UPDATER:
        return ["sh", str(Path(checkout) / "packaging/vela-update"), "--channel", "beta"]
    return ["vela-update", "--channel", "beta"]


def check_current(version):
    """The installed updater must find the published release and call it
    current. Plain --check also exits 1 when the check itself failed, so
    read the JSON (docs/updates.md §5)."""
    result = subprocess.run(["vela-update", "--channel", "beta", "--check", "--json"],
                            capture_output=True, text=True)
    print(result.stderr, end="", file=sys.stderr)
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError:
        raise UpgradeError(f"the installed updater printed no JSON (exit {result.returncode})") from None
    if report.get("status") != "up-to-date" or report.get("version") != version:
        raise UpgradeError(f"the installed updater reports {report.get('status')} for "
                           f"{report.get('version')} ({report.get('error')}), expected up-to-date for {version}")


def upgrade_with_updater(version, checkout, releases):
    name = distro(os_release())
    install_cosign(name)
    with tempfile.TemporaryDirectory(prefix="vela-previous-") as directory:
        previous = install_previous(version, name, releases, directory)
    if previous is None:
        return
    # pacman asks before installing; the updater passes the question on.
    run(*updater_command(previous, checkout), stdin="y\n" * 20)
    check_installed(name, version)
    check_current(version)
    print(f":: {name}: the updater of {previous} installed {version}, which is up to date")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("packages")
    p.add_argument("--version", required=True)
    p.add_argument("--candidate", required=True, help="folder with the candidate package")
    p = commands.add_parser("updater")
    p.add_argument("--version", required=True)
    p.add_argument("--checkout", required=True, help="checkout of the release commit")
    args = parser.parse_args(argv)
    try:
        if not re.fullmatch(r"0\.\d+\.\d+", args.version):
            raise UpgradeError(f"not a Beta version: {args.version}")
        releases = published_releases(os.environ.get("GH_TOKEN"))
        if args.command == "packages":
            upgrade_packages(args.version, args.candidate, releases)
        else:
            upgrade_with_updater(args.version, args.checkout, releases)
    except UpgradeError as error:
        print(f"::error::{error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
