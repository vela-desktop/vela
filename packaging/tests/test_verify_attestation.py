"""Fast unit tests for the policy enforced after Sigstore verification."""
from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[1] / "verify-attestation.py"
spec = importlib.util.spec_from_file_location("vela_verification", MODULE)
assert spec and spec.loader
verification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verification)


def bundle(digest: str, runner: str = "github-hosted") -> dict:
    statement = {
        "predicateType": "https://slsa.dev/provenance/v1",
        "subject": [{"name": "vela-git", "digest": {"sha256": digest}}],
        "predicate": {"buildDefinition": {"internalParameters": {
            "github": {"runner_environment": runner}}}},
    }
    return {"dsseEnvelope": {
        "payload": base64.b64encode(json.dumps(statement).encode()).decode(),
        "payloadType": "application/vnd.in-toto+json",
    }}


class VerificationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.package = Path(self.directory.name) / "vela-git.rpm"
        self.package.write_bytes(b"test binary content")
        self.digest = hashlib.sha256(self.package.read_bytes()).hexdigest()
        self.commit = "a" * 40

    def test_curl_does_not_follow_redirects(self):
        url = f"https://api.github.com/repos/vela-desktop/vela/attestations/sha256:{self.digest}"
        with patch.object(verification.subprocess, "check_output", return_value=b"{}") as output:
            verification.curl(url)
        command = output.call_args.args[0]
        self.assertEqual(command[0], "curl")
        self.assertIn("-fsS", command)
        self.assertNotIn("-L", command)
        self.assertNotIn("--location", command)
        self.assertEqual(command[-1], url)

    def test_redirect_body_cannot_reach_cosign(self):
        url = "https://api.github.com/redirected-bundle"
        with patch.object(verification, "bundle_candidates", return_value=[url]), \
             patch.object(verification.subprocess, "check_output",
                          return_value=b"<html>redirect</html>") as download, \
             patch.object(verification.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "none of the Sigstore attestations passed"):
                verification.verify(self.package, "vela-desktop/vela", self.commit, "release")
        self.assertNotIn("-L", download.call_args.args[0])
        run.assert_not_called()

    def test_valid_main_attestation_pins_workflow_and_commit(self):
        with patch.object(verification, "bundle_candidates", return_value=[bundle(self.digest)]), \
             patch.object(verification.subprocess, "run") as run:
            verification.verify(self.package, "vela-desktop/vela", self.commit, "release")
        args = run.call_args.args[0]
        self.assertIn("--certificate-identity", args)
        self.assertIn("https://github.com/vela-desktop/vela/.github/workflows/build.yml@refs/heads/main", args)
        self.assertEqual(args[args.index("--certificate-github-workflow-sha") + 1], self.commit)
        self.assertIn("--certificate-github-workflow-trigger", args)

    def test_beta_release_identity_pins_exact_tag_and_commit(self):
        with patch.object(verification, "bundle_candidates", return_value=[bundle(self.digest)]), \
             patch.object(verification.subprocess, "run") as run:
            verification.verify(self.package, "vela-desktop/vela",
                                self.commit, "release", "v0.1.0")
        args = run.call_args.args[0]
        self.assertIn("https://github.com/vela-desktop/vela/.github/workflows/build.yml@refs/tags/v0.1.0", args)
        self.assertEqual(args[args.index("--certificate-github-workflow-ref") + 1],
                         "refs/tags/v0.1.0")
        self.assertEqual(args[args.index("--certificate-github-workflow-sha") + 1], self.commit)

    def test_beta_ubuntu_release_uses_called_workflow_identity(self):
        deb = self.package.with_name("vela_0.1.0_amd64.deb")
        deb.write_bytes(self.package.read_bytes())
        with patch.object(verification, "bundle_candidates", return_value=[bundle(self.digest)]), \
             patch.object(verification.subprocess, "run") as run:
            verification.verify(deb, "vela-desktop/vela", self.commit,
                                "release", "v0.1.0")
        args = run.call_args.args[0]
        self.assertIn("https://github.com/vela-desktop/vela/.github/workflows/ubuntu.yml@refs/tags/v0.1.0", args)
        self.assertEqual(args[args.index("--certificate-github-workflow-ref") + 1],
                         "refs/tags/v0.1.0")

    def test_invalid_beta_release_tag_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "invalid Beta release tag"):
            verification.verify(self.package, "vela-desktop/vela",
                                self.commit, "release", "v0.1.0-beta.1")

    def test_self_hosted_runner_is_rejected(self):
        with patch.object(verification, "bundle_candidates", return_value=[bundle(self.digest, "self-hosted")]), \
             patch.object(verification.subprocess, "run"):
            with self.assertRaisesRegex(ValueError, "non-GitHub-hosted"):
                verification.verify(self.package, "vela-desktop/vela", self.commit, "release")

    def test_digest_does_not_match_signed_statement(self):
        with patch.object(verification, "bundle_candidates", return_value=[bundle("f" * 64)]), \
             patch.object(verification.subprocess, "run"):
            with self.assertRaisesRegex(ValueError, "none of the Sigstore attestations passed"):
                verification.verify(self.package, "vela-desktop/vela", self.commit, "release")

    def test_cosign_rejection_is_fail_closed(self):
        rejection = subprocess.CalledProcessError(1, "cosign", stderr=b"invalid signature")
        with patch.object(verification, "bundle_candidates", return_value=[bundle(self.digest)]), \
             patch.object(verification.subprocess, "run", side_effect=rejection):
            with self.assertRaisesRegex(ValueError, "invalid signature"):
                verification.verify(self.package, "vela-desktop/vela", self.commit, "release")

    def test_missing_or_nonstandard_signed_runner_is_rejected(self):
        evidence = bundle(self.digest)
        decoded = json.loads(base64.b64decode(evidence["dsseEnvelope"]["payload"]))
        del decoded["predicate"]["buildDefinition"]["internalParameters"]["github"]["runner_environment"]
        evidence["dsseEnvelope"]["payload"] = base64.b64encode(json.dumps(decoded).encode()).decode()
        with patch.object(verification, "bundle_candidates", return_value=[evidence]), \
             patch.object(verification.subprocess, "run"):
            with self.assertRaisesRegex(ValueError, "runner environment"):
                verification.verify(self.package, "vela-desktop/vela", self.commit, "release")


if __name__ == "__main__":
    unittest.main()
