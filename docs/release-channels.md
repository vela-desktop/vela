# Vela release channels

Vela publishes two channels. They update **Vela only**, not the host operating
system or packages unrelated to Vela.

| Channel | Native package | Target | Binaries |
| --- | --- | --- | --- |
| Beta | `vela` | Latest numbered `v0.x.y` release | Signed official release assets |
| Experimental | `vela-git` | Development `main` or a selected commit | Source build, or a signed CI artifact (Arch, Fedora) |

There are no release candidates. The `0.x.y` series is Beta; `1.0.0` is
the future stable milestone.

## Maintainer release process

1. Prepare the exact release commit, update `project(VERSION ...)` in
   `CMakeLists.txt`, and complete reviews and required test suites,
   including both GPU suites on that commit.
2. Rehearse on a public fork: push the same signed tag there and let the
   whole pipeline publish a release in the fork (see
   [Rollout validation](#rollout-validation)). `v0.*` tags in this
   repository can be neither moved nor deleted, so a tag whose pipeline
   fails burns that version number.
3. Create a signed, protected Git tag `v0.x.y` pointing to that commit.
4. GitHub Actions builds Arch/CachyOS, Fedora 44 and Ubuntu 26.10 packages
   for the **tagged** commit, verifies package installs and their runtime
   requirements, then publishes an official GitHub prerelease.
5. The published release includes the package files and one SHA-256 manifest
   per distribution. Each binary must be accompanied by GitHub-hosted
   cryptographic provenance that binds the build to the tag and commit.
6. If a build, smoke test or verification fails, there is no new Beta release.
7. Replace the generated notes with hand-written ones
   (`gh release edit v0.x.y --notes-file …`); editing the text leaves the
   assets untouched. Settings shows them as plain text
   ([Updates](updates.md)), so they must read well without Markdown
   rendering: full URLs rather than links, no tables.

Pushes to `main` build and test Experimental in CI but do not create
GitHub Releases. Temporary CI artifacts are **not** a public update channel.
The historical per-commit `alpha-<sha>` releases have been retired.

A Beta tag must match the project version exactly and follow the
`v0.<minor>.<patch>` format. Do not retag an already published version:
create a new patch release to correct a bad Beta.

## Release branches

`main` is the next minor version in development (the project version on
`main` is that version, e.g. `0.2.0`), and Experimental follows it. Each
published minor version has a maintenance branch, `release/0.<minor>`,
created from its `v0.<minor>.0` tag, which receives fixes only. Both are
protected the same way: changes go through pull requests and the same
required checks, and CI runs on pushes to either.

- **Fixes go to `main` first,** then to the release branch: label the
  pull request `backport-0.<minor>` and, once it is merged, open a pull
  request against `release/0.<minor>` with `git cherry-pick -x <commit>`,
  which records the original commit. A fix that only applies to the
  release branch, because `main` has changed that code, goes straight to
  it.
- **A patch release** (`v0.<minor>.<patch>`) is tagged on the release
  branch, after a pull request there sets `project(VERSION ...)` to that
  version. The rehearsal and the GPU suites run on that commit.
- **A minor release** (`v0.<minor>.0`) is tagged on `main`; its
  `release/0.<minor>` branch is then created from the tag, and `main`
  moves to the next minor version. The fallback version in
  `packaging/arch/PKGBUILD.beta`, used by manual builds, stays on the
  latest published tag.

There is one Beta channel, and the updater offers the highest published
`v0.x.y`: once `v0.<minor+1>.0` is out, Beta users move to it, so the
previous release branch only matters until then.

## Updater behavior

Install **Cosign** before installing Beta: it verifies GitHub-hosted Sigstore
attestations for the exact package, release tag and commit. Arch Beta packages
declare Cosign as a runtime dependency; on Fedora and Ubuntu, install a trusted
Cosign executable separately before starting the updater. `curl`, Git and
Python 3 are also required and are native package dependencies.

Use `vela-update --channel beta --check --json` for versioned machine-readable
Beta status and `vela-update --channel beta` to install the signed native
package. A newer installed Beta is never silently downgraded: explicitly select
an older release with `--tag=v0.x.y` when a downgrade is intended.
Use `vela-update --channel experimental` for an exact development
revision. On Arch and Fedora, a signed CI artifact for that commit can be
used through an authenticated GitHub CLI; otherwise Vela builds locally. The installed package determines the default channel; an
explicit `--channel` chooses a transition.

Cross-channel transitions replace the native package rather than installing
both simultaneously. Both packages conflict with one another, and user data
must remain outside the package payload. The installation needs native package
manager privileges and might replace a newer development commit with an older
Beta snapshot. Confirm channel changes in the Settings UI before invoking the
package manager.

The Beta updater deliberately **fails closed** if a release manifest is
missing, corrupt or lacks signed provenance. It never silently compiles a
source snapshot as an alternative to a missing official Beta binary. The
check JSON is schema-versioned (schema 2, both channels); the desktop reads
it and the state files `vela-update` writes, never its log lines.

## Settings integration

Settings > System > Updates ([Updates](updates.md)) shows both channels
through `vela-update-daemon`, which runs the checks and starts `vela-update`
in a terminal. Its channel selector asks before a transition: what the other
channel is, that the package is replaced, that the version may go back, and
that the update runs in a terminal.

## Rollout validation

Before every Beta, run the tag pipeline on a public fork of the repository
with the exact release commit. In the fork, the workflow runs from the
tagged commit, so its `main` does not need to be updated. To try again,
delete the previous release together with its tag
(`gh release delete v0.x.y --cleanup-tag`), then push the tag again:
`publish-beta` refuses to replace published bytes, and an unchanged tag
does not start the pipeline. This works because immutable releases are
off on the fork; never enable them there. Then check:

- a rerun of `publish-beta` keeps the published assets unchanged;
- each package matches its manifest and its attestation names the fork,
  the tag, the commit and the expected workflow;
- in clean Arch, Fedora 44 and Ubuntu 26.10 containers, the installed
  `vela-update-beta` with `REPO` pointed at the fork finds the release,
  replaces `vela-git` unattended, then reports `up-to-date`; the verifier
  rejects the wrong repository, the wrong commit and tampered bytes; and
  the way back to Experimental works.

For `v0.1.0` (31 October 2026) the rehearsal found two pipeline bugs that
only tag builds reach (`cosign` missing from the Arch package job, and the
channel-replacement smoke test mistaking `vela-git` for `vela` through
`provides`), fixed in [#177](https://github.com/vela-desktop/vela/pull/177)
before the real tag. Interrupted installations and missing releases were
covered by the Updates container tests ([Updates §11](updates.md#11-stages)).
