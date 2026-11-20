# Release automation

How a Beta release, above all a patch release such as `v0.2.1`, becomes a
few commands instead of a checklist. The release rules themselves do not
change: numbered `v0.<minor>.<patch>` tags, the release branches and the
validation in [release channels](release-channels.md).

> **Status:** R1 in progress: the rehearsal tag.
>
> **Remaining:** R2–R4 (§5).

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
  fork is retired after R2.
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

## 5. Stages

| Stage | What | Pull request |
|---|---|---|
| **R1** | The rehearsal tag (§4) and `release.py rehearse`/`clean`. | [#306](https://github.com/vela-desktop/vela/pull/306) |
| **R2** | Upgrade tests in CI: the previous published Beta installed in clean Arch, Fedora 44/45 and Ubuntu 26.10 containers, upgraded to the rehearsal's draft packages through the installed updater, then checked; the same test against the real release after the tag. Retires the fork. | — |
| **R3** | `release.py prepare` (version bump pull request with its CHANGELOG section), `release.py status` (rehearsal, both GPU suites and upgrade tests on the exact commit, in one go/no-go summary; the GPU suites are dispatched by `rehearse`), `release.py tag` (refuses unless `status` is green, signs and pushes). | — |
| **R4** | After publication: notes taken from the CHANGELOG section instead of `--generate-notes`, and the `PKGBUILD.beta` fallback pull requests opened by `release.py tag`. | — |

R2 has one open question. The installed updater reads releases from the
public API and skips drafts, so the upgrade test cannot point it at the
draft unchanged. The proposal is to test a copy of the installed updater
whose only changes are the repository, an accepted tag (`rehearsal/v0.x.y`)
and an authenticated download, as the 0.1.1 container tests did with
`REPO`; the unmodified updater is then exercised against the real release
right after the tag.

## 6. Not in scope

- Tags or releases created by CI on its own.
- A second Beta channel or pre-release versions such as `v0.2.1-rc1`: the
  updater would have to learn to order them, and a rehearsal already plays
  that role without reaching users.
- 0.1.x: its updater only sees prereleases, and 0.1.x support ends with
  `v0.2.0`.

## 7. Tests

- `packaging/tests/test_beta_publication.py` runs the real publication
  script with a fake `gh`: a real tag creates a normal release, a
  rehearsal creates a draft with the rehearsal title, reruns keep either
  one unchanged and reject changed manifests, and a malformed rehearsal tag
  is refused.
- `packaging/tests/test_ci_workflows.py` checks that both tags start the
  pipeline and that no job matches `refs/tags/v0.*` on its own.
- `scripts/tests/test_release.py` runs `release.py` against a temporary
  Git repository with a fake `gh`: version mismatch, an existing real tag,
  replacing an earlier rehearsal, and `clean`.
