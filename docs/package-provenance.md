# Vela Arch and Fedora package provenance and release policy

Vela publishes experimental per-commit packages for Arch and Fedora 45.
Arch assets use `SHA256SUMS`, Fedora RPMs use `SHA256SUMS-fedora`, with one
checksum entry in each. A matching filename and manifest downloaded together
are **not** proof of origin:
both files can be replaced by the same attacker. Downloaded packages must pass
cryptographic provenance verification **before** `sudo pacman -U` or
`sudo dnf install`.

## Trust model

The [Build workflow](../.github/workflows/build.yml) runs `makepkg` for Arch
and `rpmbuild` for Fedora 45, and signs the exact output files using
`actions/attest`. The signed SLSA build-provenance attestations bind each
package's SHA-256 to the workflow's GitHub OIDC identity and source commit.
Publishing checks both signatures and the exact main commit, **and** requires
clean-container installation smoke tests of the same uploaded Arch and Fedora
packages. The RPM smoke test also checks installed files and shared-library
linkage; neither test substitutes for a physical GPU login. PR-only package tests remain independent
and do not replace this release gate.

`vela-update` also verifies downloaded **public release assets** and
**GitHub Actions artifacts** with `gh attestation verify`. Verification
requires the expected repository, signer workflow
`.github/workflows/build.yml`, the precise selected 40-character source SHA,
and GitHub-hosted runners. For public alpha releases it also requires the
`refs/heads/main` source ref. A failed or missing attestation, a missing
GitHub CLI, or an unavailable verification service means **no binary install**:
the updater shows GitHub CLI's verification error and falls back to building
from the selected Git revision with native packaging tools
(`makepkg` on Arch, `rpmbuild` plus `dnf builddep` on Fedora).

Use `--build` to skip all binary download paths explicitly. Installing from
a user-selected source repository remains a trust decision: signatures
authenticate the claimed builder and revision but cannot prove that its source
code is harmless. Existing installed packages and manual `pacman -U` or `dnf install` commands
are outside the updater's verification boundary.

### Requirements for users

Install GitHub CLI (`gh`) for verified prebuilt packages; no GitHub login
should be necessary when verifying public attestations, subject to current
GitHub API and CLI requirements. If verification is unavailable, use
`vela-update --build`. Older releases created before signed attestations were
enabled cannot be trusted by the updater solely on their checksum and will be
built locally instead. No `--insecure` bypass is provided.

Example manual verification:

```sh
gh attestation verify vela-git-<version>.pkg.tar.zst \
  --repo vela-desktop/vela \
  --signer-workflow vela-desktop/vela/.github/workflows/build.yml \
  --source-digest <full-40-character-commit-sha> \
  --source-ref refs/heads/main \
  --deny-self-hosted-runners
```

Fedora uses the same verification flags, replacing the input with
`vela-git-<version>-<release>.fc45.x86_64.rpm`. Both binary types are
published atomically under the same `alpha-<full-SHA>` release, following
successful package smoke checks on each distribution.

## Required repository setting: immutable releases

**A repository administrator must enable this in GitHub.** A workflow cannot
silently configure the repository-level setting. Navigate to:

**Repository → Settings → General → Releases → Enable release immutability.**

This prevents modification of release tags and assets after publication.
It affects only *future* releases; existing alpha releases remain mutable.
See [GitHub's instructions](https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/establish-provenance-and-integrity/prevent-release-changes).

The alpha publisher attaches both packages and both checksum manifests when creating the
release (GitHub CLI prepares it as a draft and then publishes it). It retains
only the latest five alpha releases. Deleted immutable releases cannot have
their tag names reused, so do not attempt to republish pruned alpha commit
tags; build locally or use a new commit. The existing publisher intentionally
does not overwrite released assets and checks a rerun against the existing
manifest.

## Limitations

- Attestations establish *provenance and integrity*, not reproducible builds,
  vulnerability freedom, trusted dependencies, or runtime compatibility.
- The smoke test checks package installation and basic runtime/linkage, **not**
  a full Wayland login/session on a physical GPU.
- Signed provenance and the SHA256SUMS check complement each other; checksum
  verification alone is not accepted as a security guarantee.
- This policy depends on GitHub-hosted runners, GitHub's identity service and
  Sigstore. Operator credentials and malicious source changes remain threats
  requiring repository branch protection and review.

## Development and verification

```sh
python3 -m unittest discover -s packaging/arch/tests -v
sh -n packaging/arch/vela-update
python3 -m unittest discover -s packaging/arch/tests -p 'test_fedora_update.py' -v
```

Offline tests simulate successful and rejected `gh attestation verify`
results and assert fail-closed installation behavior. They **do not simulate
Sigstore cryptographic verification**; end-to-end attestations are generated
by the main-branch package job, checked again by the publisher and verified
with the real `gh` client. Both package-smoke jobs install their respective uploaded
binary and must pass before a public alpha is published.
