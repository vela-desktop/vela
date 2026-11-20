# Release automation

How a Beta release, above all a patch release such as `v0.2.1`, becomes a
few commands instead of a checklist. The release rules themselves do not
change: numbered `v0.<minor>.<patch>` tags, the release branches and the
validation in [release channels](release-channels.md).

> **Status:** R1 (the rehearsal tag) and R2 (upgrade tests) in progress.
>
> **Remaining:** R3–R4 (§6).

## 1. The problem

Publishing 0.1.1 and 0.1.2 took these manual steps, each one a chance to
forget something:

1. A pull request on the release branch that sets `project(VERSION ...)`.
2. A tag on the public fork `BrandoDev/vela-release-rehearsal`, deleting
   the fork's release and tag before every new attempt.
3. Both GPU suites, started by hand on the candidate commit.
4. Upgrade tests from the previous Beta in Arch, Fedora and Ubuntu
   containers, with scripts that lived outside the repository.
5. Release notes written by hand, because `--generate-notes` gives only a
   changelog link.
6. The signed tag.
7. Two pull requests, on `main` and on the release branch, that move the
   fallback version in `packaging/arch/PKGBUILD.beta`. This one was
   forgotten: the fallback stayed on 0.1.1 after 0.1.2 shipped.

A hotfix should not cost more than its fix. A different version format
(`0.1.2a`, `0.1.2-hotfix`) is not the answer: pacman orders `0.1.2a`
*before* `0.1.2` (`vercmp 0.1.2a 0.1.2` is `-1`), dpkg and RPM order it
after, and the publication job accepts only `v0.x.y`. The patch number is
what a hotfix is for; what has to shrink is the work around it.

## 2. Goals

1. **One command per decision.** Preparing a release, rehearsing it and
   tagging it are the maintainer's decisions; everything between them runs
   by itself.
2. **The same pipeline.** The rehearsal runs the exact jobs a real tag runs,
   in the same repository, so a pipeline bug shows up before the tag burns
   a version number (`v0.*` tags and published releases are immutable).
3. **Nothing reaches users by mistake.** A rehearsal can never be offered by
   the updater, never becomes the latest release and never needs a ruleset
   exception.
4. **Signatures stay with people.** CI never holds a key that signs tags.

## 3. Decisions taken

- **The maintainer signs the tag** with their SSH key, through
  `scripts/release.py tag` (R3), which first checks that the candidate
  passed. No signing key or bot identity is stored in CI.
- **The rehearsal runs in `vela-desktop/vela`,** not on a fork: a
  `rehearsal/v0.<minor>.<patch>` tag runs the Beta pipeline and publishes
  a *draft* release. No token for another repository is needed, and the
  fork is retired with R2.
- **No modified updater in the tests.** Before the tag the packages are
  upgraded with the native package manager; the updater itself is tried,
  unmodified, against the real release right after it is published (§5).
- **Version numbers stay `v0.<minor>.<patch>`.** A hotfix is a patch
  release.

## 4. The rehearsal tag

`rehearsal/v0.2.1` on the candidate commit starts `build.yml` like
`v0.2.1` would. The `changes` job derives the Beta version from the tag
(`0.2.1`, the same for both) and every job reads it from there instead of
matching `refs/tags/v0.*` itself, so the two tags cannot take different
paths through the jobs:

- the Arch, Fedora and Ubuntu packages are built as the `vela` Beta
  package, with the version checked against `project(VERSION ...)`;
- the clean-install and channel-replacement smoke tests are the Beta ones;
- the packages are attested and `publish-beta` verifies the attestations
  against the rehearsal tag and commit.

Only the last step differs. `publish-beta` creates a **draft** release
titled "Rehearsal of Vela 0.2.1 Beta", with the same packages and
manifests. A draft:

- is never seen by the updater, which skips drafts and accepts only tags
  matching `v0.x.y` (`rehearsal/v0.2.1` does not);
- is not immutable, so it can be deleted with its tag and the rehearsal
  run again;
- is reached only by maintainers, who can download and inspect its
  packages.

A rerun of `publish-beta` on the same rehearsal checks that the existing
draft has the same manifests, as it does for a real release.

The rulesets cover only `refs/tags/v0.*`. `rehearsal/*` tags can be
created and deleted by anyone with write access, and only maintainers have
it.

`scripts/release.py rehearse 0.2.1` checks the candidate and pushes the
tag; `scripts/release.py clean 0.2.1` deletes the draft and the tag. The
candidate is the tip of `origin/release/0.2`, or `origin/main` for a
`.0` release, unless `--commit` names another; its `project(VERSION ...)`
must be the version being rehearsed, and the real `v0.2.1` tag must not
exist yet.

## 5. Upgrade tests

`scripts/ci-beta-upgrade.py` starts from what users have: the newest
published Beta older than the version under test, downloaded from its
release and checked against the release's manifest. A release without that
distribution's manifest (0.1.0 and 0.1.1 had no Fedora 45 package) is
skipped; when nothing older exists, there is nothing to upgrade from and
the test says so. It runs in clean Arch, Fedora 44, Fedora 45 and Ubuntu
26.10 containers.

**Before publication** (`beta-upgrade` in `build.yml`, on the release tag
and on its rehearsal), the previous Beta is upgraded to the run's own
packages with pacman, dnf or apt, the packages that publication would
upload. Then the installed version must be the new one, the package's
files must verify (`pacman -Qkk`, `rpm -V`, `dpkg --verify`) and
`vela-update` must run. `publish-beta` waits for all four.

**After publication** (`beta-update-check.yml`, called once `publish-beta`
has published a real release, and runnable by hand for any published
Beta), the previous Beta's own updater installs the release, as it does for
users. From an updater before 0.2.0, which only sees prereleases, the test
takes the documented one-time step, `packaging/vela-update` from the
release's tree ([release channels](release-channels.md#updating-older-beta-installations)).
Then `vela-update --check --json` must report the new release as
`up-to-date`. It reads the JSON because plain `--check` also exits 1 when
the check itself fails, and an updater that finds no release at all (0.1.x
once its releases became normal ones) must not pass.

What this does not cover before the tag is the updater's conversation with
the real GitHub. The updater cannot read a draft, and a copy patched to
read one (tag format, downloads, provenance reference) would need changing
for every old updater. Its logic stays covered offline by
`packaging/tests`, and a failure after publication is fixed by a patch
release.

## 6. Stages

| Stage | What | Pull request |
|---|---|---|
| **R1** | The rehearsal tag (§4) and `release.py rehearse`/`clean`. | [#306](https://github.com/vela-desktop/vela/pull/306) |
| **R2** | Upgrade tests in CI (§5): the previous Beta upgraded to the candidate packages before publication, and its updater installing the real release after it. Retires the fork. | [#307](https://github.com/vela-desktop/vela/pull/307) |
| **R3** | `release.py prepare` (version bump pull request with its CHANGELOG section), `release.py status` (rehearsal, both GPU suites and upgrade tests on the exact commit, in one go/no-go summary; the GPU suites are dispatched by `rehearse`), `release.py tag` (refuses unless `status` is green, signs and pushes). | — |
| **R4** | After publication: notes taken from the CHANGELOG section instead of `--generate-notes`, and the `PKGBUILD.beta` fallback pull requests opened by `release.py tag`. | — |

## 7. Not in scope

- Tags or releases created by CI on its own.
- A second Beta channel or pre-release versions such as `v0.2.1-rc1`: the
  updater would have to learn to order them, and a rehearsal already plays
  that role without reaching users.
- 0.1.x: its updater only sees prereleases, and 0.1.x support ends with
  `v0.2.0`.

## 8. Tests

- `packaging/tests/test_beta_publication.py` runs the real publication
  script with a fake `gh`: a real tag creates a normal release, a
  rehearsal creates a draft with the rehearsal title, reruns keep either
  one unchanged and reject changed manifests, and a malformed rehearsal tag
  is refused.
- `packaging/tests/test_ci_workflows.py` checks that both tags start the
  pipeline and that no job matches `refs/tags/v0.*` on its own.
- `scripts/tests/test_ci_beta_upgrade.py` checks the choice of the
  previous Beta (numeric order; drafts, rehearsals and other tags skipped;
  a missing manifest), the supported systems, the candidate package names,
  when the one-time step applies, refused downloads, and the final verdict:
  only `up-to-date` for the new version passes, and a failed check does not.
- `packaging/tests/test_ci_workflows.py` also checks that `beta-upgrade`
  covers the four systems and gates `publish-beta`, and that the updater
  check runs after real releases only.
- `scripts/tests/test_release.py` runs `release.py` against a temporary
  Git repository with a fake `gh`: version mismatch, an existing real tag,
  replacing an earlier rehearsal, and `clean`.
