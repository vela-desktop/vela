# Additional CI quality checks

The main Build workflow runs on pushes to `main`, pull requests and manual dispatch; it also invokes the Fedora build and test workflow to gate public releases.
Sanitizer Tests and Static Analysis run separately on pull requests and manual dispatch;
Standalone Package Test is now manual-only because Build already performs the
same Arch package build and smoke test on PRs. Documentation-only PRs skip the
expensive Build jobs after an inexpensive source-change check.

## Package and updater contracts

The [Build workflow](../.github/workflows/build.yml) runs the offline
`packaging/tests` suite before the full build. The regular Arch build excludes
CTest's `packaging-update` to avoid running that same suite twice. It checks public HTTPS defaults, migration of old
SSH configuration and mirrors, revision selection, CI artifact selection and
failure handling. It uses real Git with isolated configuration and rejects
unexpected transports; package tools are simulated. See [testing.md](testing.md)
for the local command and test isolation details.

## Static analysis

[Static Analysis](../.github/workflows/static-analysis.yml) builds Vela's C
compositor using Clang Static Analyzer (scan-build --status-bugs) and checks
CPU-independent C modules with cppcheck and clang-tidy. No GPU is necessary.

## Sanitizers

[Sanitizer Tests](../.github/workflows/sanitizers.yml) builds with
AddressSanitizer and UndefinedBehaviorSanitizer and runs CPU-only CTest
suites. It deliberately excludes functional and sharpness tests, which
need a real GPU exposing Vulkan 1.4 and a DRM render node.

## Standalone package test (manual only)

[Standalone Package Test](../.github/workflows/package-smoke.yml) builds
the existing PKGBUILD from the checked-out revision, then installs its
artifact into a separate fresh Arch container.
[ci-check-package.sh](../scripts/ci-check-package.sh) verifies files,
runtime linking, and no required Plasma/GNOME Shell installation.
This is a package smoke test, NOT a graphical login or Vulkan test.

## Manual real-GPU CI (self-hosted AMD)

[GPU CI - AMD (manual)](../.github/workflows/gpu-amd.yml) is **only**
triggered via `workflow_dispatch`; no push or pull request starts it.
The runner needs GitHub's standard `self-hosted`, `linux`, `x64` labels
and an additional custom **`vela-amd`** label (a runner name alone does
not supply this label). Register it under Settings → Actions → Runners.

The CachyOS/Arch runner must have the normal
[build dependencies](development.md#distribution-dependencies), plus
`vulkan-tools`, `vulkan-radeon`, `python-numpy` and `python-pillow`.
It must be able to access an AMD DRM render node and the RADV Vulkan 1.4
driver. Install dependencies **on the host**, not inside the workflow;
the workflow deliberately runs without `sudo` or host package upgrades.
It uses a private XDG runtime directory and CMake build directory within
the runner's temporary workspace, then runs the existing
[GPU-labeled CTest suites](testing.md#run-everything)
(functional, sharpness and sharpness-cpu). It rejects missing or skipped
tests instead of reporting a false hardware pass, and uploads diagnostic
logs and JUnit XML.

After the workflow is present on the default branch, go to Actions →
GPU CI - AMD (manual) → Run workflow to choose a **trusted** branch.
Do not dispatch unreviewed pull request code on a persistent self-hosted
runner: checkout, build scripts and tests execute code locally. Prefer
a dedicated unprivileged runner account without access to personal files
or secrets; never give the runner passwordless sudo. The GPU suite starts
isolated headless Wayland sessions, not a physical DRM/KMS test.

## Prerequisites

GitHub Actions must be enabled. The normal automatically triggered CI needs
no self-hosted runner or secrets; the optional manual AMD GPU workflow requires
the explicitly labeled local runner above. Required check / branch protection
policy is a separate optional admin setting.
