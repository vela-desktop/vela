# Vela release channels

Vela publishes two channels. They update **Vela only**, not the host operating
system or packages unrelated to Vela.

| Channel | Native package | Target | Binaries |
| --- | --- | --- | --- |
| Beta | `vela` | Latest numbered `v0.x.y` release | Signed official release assets |
| Experimental | `vela-git` | Development `main` or an explicitly selected commit | Build locally; historical signed `alpha-<sha>` packages or eligible temporary CI artifacts may still be used |

There are no release candidates. The `0.x.y` series is Beta; `1.0.0` is
the future stable milestone, not part of this initial change.

## Maintainer release process

1. Prepare the exact release commit, update `project(VERSION ...)` in
   `CMakeLists.txt`, and complete reviews and required test suites.
2. Create a protected Git tag `v0.x.y` pointing to that commit.
3. GitHub Actions builds Arch/CachyOS, Fedora 44 and Ubuntu 26.10 packages
   for the **tagged** commit, verifies package installs and their runtime
   requirements, then publishes an official GitHub prerelease.
4. The published release includes the package files and one SHA-256 manifest
   per distribution. Each binary must be accompanied by GitHub-hosted
   cryptographic provenance that binds the build to the tag and commit.
5. If a build, smoke test or verification fails, there is no new Beta release.

Pushes to `main` continue to build/test Experimental in CI but no longer
create public per-commit GitHub Releases. Temporary CI artifacts are **not**
a public update channel. Historical `alpha-<sha>` releases must be retired
separately after active installs have migrated.

A Beta tag must match the project version exactly and follow the
`v0.<minor>.<patch>` format. Do not retag an already published version:
create a new patch release to correct a bad Beta.

## Updater behavior

Install **Cosign** before installing Beta: it verifies GitHub-hosted Sigstore
attestations for the exact package, release tag and commit. Arch Beta packages
declare Cosign as a runtime dependency; on Fedora and Ubuntu, install a trusted
Cosign executable separately before starting the updater. `curl`, Git and
Python 3 are also required and are native package dependencies.

Use `vela-update --channel beta --check --json` for versioned machine-readable
Beta status and `vela-update --channel beta` to install the signed native
package. A newer installed Beta is never silently downgraded: explicitly select
an older release with `--tag=v0.x.y` when a downgrade is intended. Use `vela-update --channel experimental` to build an exact development
revision locally. The installed package determines the default channel; an
explicit `--channel` chooses a transition.

Cross-channel transitions replace the native package rather than installing
both simultaneously. Both packages conflict with one another, and user data
must remain outside the package payload. The installation needs native package
manager privileges and might replace a newer development commit with an older
Beta snapshot. Confirm channel changes in the Settings UI before invoking the
package manager.

The Beta updater deliberately **fails closed** if a release manifest is
missing, corrupt or lacks signed provenance. It never silently compiles a
source snapshot as an alternative to a missing official Beta binary. Release
check JSON is schema-versioned; future Settings UI changes should consume
the structured output instead of scraping human-readable log lines.

## Settings integration

[PR #133](https://github.com/vela-desktop/vela/pull/133) owns the Updates
page and should add a Beta/Experimental selector on top of the CLI contract
in this PR. Keep UI/controller changes there to avoid two concurrent
implementations of the same page.

## Rollout validation

Before merging, test channel replacements on actual clean Arch, Fedora 44 and
Ubuntu 26.10 images (including downgrades from development snapshots), signature failures, interrupted installations and missing releases. Verify
that a release workflow rerun only accepts unchanged published assets. Complete a rehearsal
with a versioned test tag before cutting the first user-facing Beta.
