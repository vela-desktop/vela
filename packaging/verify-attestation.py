#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify GitHub artifact attestations without GitHub CLI.

GitHub's public REST endpoint only locates Sigstore bundles; no HTTP metadata
is trusted. Cosign verifies the bundle cryptographically, binds the artifact
digest and enforces the OIDC workflow identity and exact source commit.
"""
from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit

ISSUER = "https://token.actions.githubusercontent.com"


def curl(url: str) -> bytes:
    """Fetch attestation data. Redirects are never followed (no -L), so only
    the hosts bundle_candidates allows are ever contacted."""
    return subprocess.check_output(
        ["curl", "-fsS", "--retry", "1", "--connect-timeout", "5",
         "--max-time", "30", url], stderr=subprocess.PIPE
    )


def bundle_candidates(repository: str, digest: str) -> list[object]:
    url = f"https://api.github.com/repos/{repository}/attestations/sha256:{digest}?per_page=100"
    response = json.loads(curl(url))
    records = response.get("attestations")
    if not isinstance(records, list) or not records:
        raise ValueError("no published attestations found for this package")
    # A truncated list could conceal the trusted attestation; never silently
    # conclude that an unverified candidate is trustworthy.
    if len(records) >= 100:
        raise ValueError("attestation list exceeds the supported page size")
    candidates = []
    for record in records:
        if not isinstance(record, dict):
            continue
        if isinstance(record.get("bundle"), dict):
            candidates.append(record["bundle"])
        url = record.get("bundle_url")
        if isinstance(url, str):
            parsed = urlsplit(url)
            host = (parsed.hostname or "").lower()
            if (parsed.scheme == "https" and not parsed.username
                    and not parsed.password and parsed.port in (None, 443)
                    and (host.endswith(".blob.core.windows.net")
                         or host in ("github.com", "api.github.com")
                         or host.endswith(".githubusercontent.com"))):
                candidates.append(url)
    if not candidates:
        raise ValueError("GitHub returned no usable Sigstore bundles")
    return candidates


def validate_signed_provenance(bundle: dict, digest: str) -> None:
    """Apply source/runner policy to the envelope after Cosign validates it."""
    envelope = bundle.get("dsseEnvelope")
    if not isinstance(envelope, dict) or not isinstance(envelope.get("payload"), str):
        raise ValueError("verified bundle has no signed DSSE statement")
    try:
        statement = json.loads(base64.b64decode(envelope["payload"], validate=True))
    except (ValueError, TypeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid signed SLSA statement") from exc
    if statement.get("predicateType") != "https://slsa.dev/provenance/v1":
        raise ValueError("unexpected provenance predicate")
    subjects = statement.get("subject")
    if not isinstance(subjects, list) or not any(
        isinstance(sub, dict) and isinstance(sub.get("digest"), dict)
        and sub["digest"].get("sha256") == digest for sub in subjects
    ):
        raise ValueError("signed provenance does not bind the package SHA-256")
    try:
        github = statement["predicate"]["buildDefinition"]["internalParameters"]["github"]
        runner = github["runner_environment"]
    except (KeyError, TypeError) as exc:
        raise ValueError("signed provenance lacks the runner environment") from exc
    if runner != "github-hosted":
        raise ValueError("refusing attestation from non-GitHub-hosted runner")


@contextmanager
def package_snapshot(package: Path):
    """Confine input to the working directory and pin a private copy for Cosign."""
    root = Path.cwd().resolve(strict=True)
    selected = package.absolute()
    selection_before = selected.lstat()
    package = package.resolve(strict=True)
    try:
        relative = package.relative_to(root)
    except ValueError as exc:
        raise ValueError("package must be inside the current working directory") from exc
    if not relative.parts:
        raise ValueError("package must be a regular file")
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    parent = os.open(root, flags)
    try:
        for component in relative.parts[:-1]:
            child = os.open(component, flags, dir_fd=parent)
            os.close(parent)
            parent = child
        fd = os.open(relative.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
                     dir_fd=parent)
    finally:
        os.close(parent)
    with os.fdopen(fd, "rb") as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError("package must be a regular file")
        with tempfile.TemporaryDirectory(prefix="vela-package-verification-") as temp:
            # A fixed filename, independent of CLI input, also rules out option
            # injection and verifies the same bytes that were hashed.
            snapshot = Path(temp) / ("package.deb" if package.suffix == ".deb" else "package.bin")
            digest = hashlib.sha256()
            with snapshot.open("xb") as target:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
                    target.write(chunk)
            yield snapshot, digest.hexdigest()
            # The updater installs the original download after this helper
            # returns. Reject in-place edits and pathname replacement as well.
            expected = (before.st_dev, before.st_ino, before.st_size,
                        before.st_mtime_ns, before.st_ctime_ns)
            for current in (os.fstat(source.fileno()), package.lstat(), selected.stat()):
                if (current.st_dev, current.st_ino, current.st_size,
                        current.st_mtime_ns, current.st_ctime_ns) != expected:
                    raise ValueError("package changed during signature verification")
            # resolve() follows an in-directory symlink. The caller still uses
            # its original spelling, so check that entry as well as its target.
            selection_after = selected.lstat()
            if (selection_after.st_dev, selection_after.st_ino, selection_after.st_size,
                    selection_after.st_mtime_ns, selection_after.st_ctime_ns) != (
                    selection_before.st_dev, selection_before.st_ino, selection_before.st_size,
                    selection_before.st_mtime_ns, selection_before.st_ctime_ns):
                raise ValueError("package pathname changed during signature verification")


def verify(package: Path, repository: str, commit: str, kind: str, release_tag: str | None = None) -> None:
    """Verify a package against GitHub-hosted Sigstore provenance and source identity."""
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid GitHub repository")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("expected the full selected commit SHA")
    if kind not in ("release", "workflow"):
        raise ValueError("invalid verification kind")
    with package_snapshot(package) as (snapshot, digest):
        verify_snapshot(snapshot, digest, repository, commit, kind, release_tag)


def verify_snapshot(package: Path, digest: str, repository: str, commit: str,
                    kind: str, release_tag: str | None) -> None:
    """Only private, fixed-name snapshots reach the external verifier."""
    workflow = "ubuntu.yml" if package.name.endswith(".deb") else "build.yml"
    identity = f"https://github.com/{repository}/.github/workflows/{workflow}"
    shared = [
        "--certificate-oidc-issuer", ISSUER,
        "--certificate-github-workflow-repository", repository,
        "--certificate-github-workflow-sha", commit,
    ]
    if kind == "release":
        if release_tag is None:
            # Legacy alpha builds were signed from main. Keep verification
            # compatible for already-published packages during migration;
            # no new alpha releases are published by the Beta pipeline.
            ref = "refs/heads/main"
        elif re.fullmatch(r"v0\.[0-9]+\.[0-9]+", release_tag):
            ref = "refs/tags/" + release_tag
        else:
            raise ValueError("invalid Beta release tag")
        shared.extend(["--certificate-identity", identity + "@" + ref,
                       "--certificate-github-workflow-ref", ref,
                       "--certificate-github-workflow-trigger", "push"])
    else:
        shared.extend(["--certificate-identity-regexp",
                       "^" + re.escape(identity) + r"@refs/heads/[A-Za-z0-9_./-]+$"])
    errors = []
    for candidate in bundle_candidates(repository, digest):
        try:
            with tempfile.TemporaryDirectory(prefix="vela-attestation-") as temp:
                bundle = Path(temp) / "bundle.json"
                if isinstance(candidate, dict):
                    evidence = candidate
                else:
                    evidence = json.loads(curl(candidate))
                bundle.write_text(json.dumps(evidence), encoding="utf-8")
                # The build's SLSA provenance: cosign 3 checks the predicate
                # type, and its default ("custom") matches no attestation.
                command = ["cosign", "verify-blob-attestation",
                           "--bundle", str(bundle), "--type", "slsaprovenance1",
                           *shared, "--", str(package)]
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=90)
                validate_signed_provenance(evidence, digest)
                return
        except subprocess.CalledProcessError as exc:
            message = (exc.stderr or b"").decode("utf-8", "replace").strip()
            errors.append(message[:400] or str(exc))
        except (subprocess.TimeoutExpired, OSError, ValueError) as exc:
            errors.append(str(exc))
    raise ValueError("none of the Sigstore attestations passed verification: "
                     + "; ".join(errors[:3]))


def main() -> int:
    """Validate CLI parameters and fail closed when any attestation check fails."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--kind", required=True, choices=("release", "workflow"))
    parser.add_argument("--release-tag", default=None)
    parser.add_argument("package", type=Path, help="regular package file inside the current directory")
    args = parser.parse_args()
    try:
        verify(args.package, args.repository, args.commit, args.kind, args.release_tag)
    except (ValueError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired, json.JSONDecodeError) as exc:
        print(f"vela-update: attestation verification failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
