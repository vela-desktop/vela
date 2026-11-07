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

## Codecov (first-party C/C++ coverage: CPU and optional GPUs)

The [CPU coverage workflow](../.github/workflows/coverage.yml) compiles
instrumented C/C++ and runs existing CPU tests. The shared
[collector](../scripts/coverage-report.sh) excludes `tests/` directories
(including component-specific test code) and generated Qt/CMake sources,
while keeping first-party production sources, even when untested (0%).
The [summary tool](../scripts/coverage_summary.py) deduplicates source line
numbers and reports execution hits for each Vela subsystem in Actions
step summaries, as Markdown and JSON artifacts, alongside Cobertura XML.
Codecov can report different percentages when displaying partial branches.
Python and QML need different coverage tools and are not counted here.

### GPU coverage (opt-in)

Existing [AMD](../.github/workflows/gpu-amd.yml) and
[NVIDIA](../.github/workflows/gpu-nvidia.yml) workflows remain **manual**:
the standard RelWithDebInfo GPU suites run unchanged by default. Their
`collect_coverage` workflow-dispatch input (default **false**) selects a
fresh GCC Debug `--coverage` build instead and runs the same functional,
sharpness and sharpness-cpu GPU suites on the actual hardware. Coverage
is collected only after the existing no-skips/no-missing-tests check succeeds,
and only for a full run (no **tests** or **test_filter**). The instrumented
build exports `VELA_GPU_COVERAGE=1`: tests skip their throughput floors there,
which would measure gcov rather than Vela.

**Runner prerequisites:** install GCC/G++, `gcov` and `gcovr` on the AMD and
NVIDIA host systems in addition to their existing build/test dependencies.
On Arch/CachyOS the host administrator can use
`sudo pacman -S --needed gcc gcovr` before dispatching. Workflows do not
invoke sudo, upgrade system packages or change runner credentials.

**To use:** Actions → GPU CI - AMD (manual) or GPU CI - NVIDIA (manual)
→ Run workflow → select a **trusted, reviewed** branch/SHA (prefer main)
→ check **Collect coverage**. The hardware workflow exports
`vela-amd-coverage` / `vela-nvidia-coverage` as downloadable artifacts.
A separate **GitHub-hosted job** downloads the XML and uploads it to
Codecov with OIDC, so `id-token: write` does not grant privileges to the
self-hosted GPU runner. Both workflows retain their original strict GPU
hardware qualification, tests, and diagnostic artifacts.

Codecov [flags](../.codecov.yml) are `cpu-tests`, `gpu-amd` and
`gpu-nvidia`; they can be compared separately, and uploads for the
**same exact Git SHA** can contribute to combined coverage. **No GPU
result is carried forward** to later commits (all `carryforward: false`).
An ordinary PR without GPU instrumentation therefore represents CPU-only
execution. Do not compare mixed CPU+GPU totals across inconsistent commits
as if the test suites were identical. Instrumented Debug builds add runtime
overhead and are not GPU performance measurements; headless tests are not
physical DRM/KMS coverage.

Codecov's public OSS tier supports uploads for this repository. The CPU
workflow and hosted GPU upload jobs use OIDC and do not need a
`CODECOV_TOKEN`. Coverage statuses are informational, and no mandatory
threshold is introduced. The collector retains
`--include-internal-functions` as a workaround for a `gcovr 8.7` crash;
function counts can therefore include compiler-generated internals.

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
