# Code quality and contribution integrations

Vela uses GitHub Actions for build, CodeQL, clang static analysis, clang-tidy,
cppcheck, OSV, sanitizers and GPU tests. These integrations add **different**
signals rather than duplicating that existing analysis.

## GitHub Labeler (fully configured in-repository)

The path-based labeler is a GitHub Action defined in
[`.github/workflows/labeler.yml`](../.github/workflows/labeler.yml), with area
mapping in [`.github/labeler.yml`](../.github/labeler.yml). It labels PRs
without checking out untrusted code; only PR metadata and the trusted base
branch configuration are read. It needs the token's `issues: write` permission
to create any as-yet missing labels, and `pull-requests: write` to apply them.
Existing manual labels are preserved. It starts working after the workflow is
merged to the default branch; the PR introducing it is not labeled retroactively.

## Codecov (code coverage, not static analysis)

The [Code coverage](../.github/workflows/coverage.yml) workflow uses the same
Arch Linux base as the normal build, instruments C/C++ code with GCC coverage,
executes the CPU-test suite, generates a Cobertura XML file with `gcovr`, and
uploads it using Codecov's GitHub OIDC authentication (no repository upload
token is needed). The XML is also retained as a workflow artifact. It is
non-blocking as a third-party upload during initial onboarding.

**Owner action:** [sign in to Codecov](https://app.codecov.io/) with GitHub,
enable `vela-desktop/vela` for the public open-source repository, and install
the [Codecov GitHub App](https://github.com/apps/codecov) if prompted to enable
PR comments/checks. Verify the first `main` and PR uploads are accepted. No
`CODECOV_TOKEN` secret is required for the configured OIDC method.

Only code that is compiled *and executed* by instrumented tests registers as
covered. The coverage report excludes generated Qt/CMake sources in
`build-coverage/`. The current Arch `gcovr 8.7` crashes in its internal-function
exclusion pass on this project, so the workflow uses
`--include-internal-functions` as a transparent workaround. This may include
compiler-generated internal functions in function-level statistics for otherwise
included first-party source files; do not interpret those percentages as a pure
developer-authored-function metric. This flag can be removed after the upstream
failure is fixed and the baseline is rechecked. Tests requiring actual
AMD/NVIDIA hardware are not exercised on GitHub-hosted runners, so these
reports must not be presented as GPU coverage.
Do not make percentage thresholds or Codecov status a merge requirement until
baseline reports and first-party paths have been inspected.

Codecov has an [open-source plan](https://about.codecov.io/pricing/) with
unlimited public repository uploads.

## SonarQube Cloud (maintainability + quality gate)

SonarQube Cloud adds code-smell/maintainability checks, duplication review,
reliability and a configurable new-code quality gate. Some findings may overlap
with CodeQL or clang-tidy, but its quality and duplication views provide an
independent reason to evaluate it. Keep CodeQL, cppcheck, scan-build and
clang-tidy enabled; compare the first batch of Sonar findings for noise
before setting a mandatory quality gate.

**Owner action:** install
[SonarQube Cloud's GitHub App](https://github.com/marketplace/sonarcloud)
for `vela-desktop/vela`, create/import the **public** project in
[SonarQube Cloud](https://sonarcloud.io/), select the free OSS plan, and use
**Automatic analysis** for C/C++. No additional GitHub Actions scan,
`SONAR_TOKEN` or compilation-database job is added here because that would
duplicate analysis and CI work. If automatic analysis proves unsuitable,
revisit CI-based analysis deliberately, not in parallel.

SonarQube's [OSS plan](https://docs.sonarsource.com/sonarqube-cloud/administering-sonarcloud/managing-subscription/subscription-plans)
supports free public project analysis and pull request analysis.
Installation and organization authorization cannot be completed by a repository
commit alone.

## DCO Bot (commit origin attestation)

Vela already requires
[`Signed-off-by`](../CONTRIBUTING.md#developer-certificate-of-origin)
on each contribution. The [.github/dco.yml](../.github/dco.yml) file enables
*individual author* remediation commits so existing unsigned commits can be
certified without force-pushing and rewriting a contributor's branch.

**Owner action:** install the
[DCO GitHub App](https://github.com/apps/dco) for the repository. The app
creates a DCO check once installed; this check can fail on **existing** open
PRs with missing sign-offs. Keep it out of required branch-protection checks
until their authors have signed off or supplied the documented remediation.
Do not enable third-party sign-off or disable member verification by default.

## Rollout and verification

1. Merge the workflow/configuration PR. Labeler will run automatically for
   subsequent PR events; confirm a changed `compositor/**` file receives
   `area: compositor`, including for a forked PR.
2. Link the public Codecov project and confirm a baseline XML artifact plus
   an accepted OIDC upload; check the measured files and coverage scope.
3. Install SonarQube Cloud, verify automatic C/C++ analysis on `main` and
   pull requests, and compare its unique findings with existing scanners.
4. Install DCO and reconcile existing unsigned PRs. Only after that should
   the DCO check be required by GitHub branch protection.

Installing third-party apps or authorizing their access requires an
organization owner's explicit action on GitHub or the service website.
