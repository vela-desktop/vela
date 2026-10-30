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
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit

ISSUER = "https://token.actions.githubusercontent.com"


def curl(url: str) -> bytes:
    """Fetch attestation data without following redirects outside allowed hosts."""
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


def verify(package: Path, repository: str, commit: str, kind: str, release_tag: str | None = None) -> None:
    """Verify a package against GitHub-hosted Sigstore provenance and source identity."""
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid GitHub repository")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("expected the full selected commit SHA")
    if not package.is_file():
        raise ValueError("package does not exist")
    import hashlib
    h = hashlib.sha256()
    with package.open("rb") as binary:
        for chunk in iter(lambda: binary.read(1024 * 1024), b""):
            h.update(chunk)
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
    for candidate in bundle_candidates(repository, h.hexdigest()):
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
                           *shared, str(package)]
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=90)
                validate_signed_provenance(evidence, h.hexdigest())
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
    parser.add_argument("package", type=Path)
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
