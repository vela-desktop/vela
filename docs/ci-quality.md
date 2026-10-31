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

## Shared hardware CI requirements

Both GPU workflows use pinned Node.js 24-based GitHub Actions and require a
**GitHub Actions self-hosted runner >= 2.327.1**. Update the runner before
dispatching either workflow. Their hardware test jobs set
`VELA_GPU_CI_STRICT=1`, so even individual Python `unittest` skips and
empty suites fail CTest (not only CTest-level skips). They also set
`VELA_GPU_EXPECT_VENDOR` to check the DRM render node actually selected
by Vela against its sysfs PCI vendor in headless functional and sharpness
sessions; passing `vulkaninfo` alone is not sufficient.

Install all dependencies needed by the GPU scenarios, including Xwayland,
`dbus-daemon`, `python-dbus` (the Python `dbus` module), `gdbus` (from
GLib) and `VK_LAYER_KHRONOS_validation` (usually the
`vulkan-validation-layers` package), in addition to the normal build packages.
Missing optional test prerequisites are an error in strict hardware CI, not a
silent pass.

The dedicated GPU workflows set `VELA_SHUTDOWN_STRESS_CYCLES=30` and
`VELA_SHUTDOWN_VALIDATE=1`. The shutdown functional tests repeat 30
live-window logout cycles, check supervised exit without a compositor restart,
and require Vulkan validation to be enabled during those tests. The validation
check fails if the requested layer is missing. These are GPU-backed **headless**
tests: passing them does not certify direct KMS, real display power cycling or
Intel ANV. See [the physical hardware matrix](hardware.md#gpu-resource-cleanup-and-driver-regression-checks).

## GPU CI policy

This is the **normative source** for when self-hosted GPU CI must run.
Other documents link here instead of restating the requirements.

- **Required before merging GPU-affecting changes:** obtain successful full
  GPU-labeled CTest suites from **both** manually dispatched workflows, AMD
  and NVIDIA, on the **exact candidate commit SHA**. This covers compositor,
  renderer, Vulkan/DRM, synchronization, and GPU-dependent tests, including
  changes that could alter GPU behavior indirectly. Do not treat a passing
  run on an older commit or a merely similarly named branch as evidence.
- **Exempt from GPU dispatch:** documentation-only, translation-only, and
  other changes confirmed to have no GPU impact. Ordinary hosted CI still
  applies. An ambiguous change is treated as GPU-affecting until reviewed.
- **Required before every release:** both GPU suites must pass on the
  exact release candidate revision, even if its last changes were only
  documentation or packaging. Known reproducible failures are release
  blockers until fixed, explicitly retested and reclassified.
- **Pass means:** the workflows actually build the selected revision on
  the expected hardware, run the full `gpu`-labeled CTest suite, and reject
  missing, failed or skipped test cases. `vulkaninfo` by itself is not
  a pass; a headless GPU pass does not certify physical DRM/KMS, VT
  switching, screen sharing, or suspend/resume.
- **Runner safety:** only manually dispatch trusted, reviewed code to
  personal self-hosted machines; use unprivileged runner accounts with no
  personal credentials, privileged sudo, or writable secrets.

The two runners are AMD Radeon RX 9070 XT (Mesa/RADV, CachyOS) and NVIDIA
GeForce RTX 4070 Laptop GPU (proprietary driver, Arch/CachyOS).

**Enforcement today:** this is a maintainer/release policy, **not yet an
automatic GitHub branch-protection gate**. Both workflows currently use
`workflow_dispatch` only. A future automated PR status check could
validate completed run conclusions, their exact commit SHAs, coverage and
exemptions, and report a required pass/fail result. Do not label the current
manual policy as an automatically enforced merge requirement.

## Manual real-GPU CI (self-hosted AMD)

[GPU CI - AMD (manual)](../.github/workflows/gpu-amd.yml) is **only**
triggered via `workflow_dispatch`; no push or pull request starts it.
The runner needs GitHub's standard `self-hosted`, `linux`, `x64` labels
and an additional custom **`vela-amd`** label (a runner name alone does
not supply this label). Register it under Settings → Actions → Runners.

The CachyOS/Arch runner must have the normal
[build dependencies](development.md#distribution-dependencies), plus
`vulkan-tools`, `vulkan-radeon`, `vulkan-validation-layers`, `python-numpy`, `python-pillow`,
`python-dbus` and the X11 tools of the native-resolution tests (`xorg-xwayland`,
`xorg-xrandr`, `xorg-xrdb`, `xorg-xprop`).
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

## Manual real-GPU CI (self-hosted NVIDIA)

[GPU CI - NVIDIA (manual)](../.github/workflows/gpu-nvidia.yml) is a
separate `workflow_dispatch` workflow targeting the `vela-nvidia`
custom runner label (plus `self-hosted`, `linux` and `x64`).
The runner group must allow the public Vela repository to use it.

The Arch/CachyOS runner needs the
[normal build dependencies](development.md#distribution-dependencies),
`vulkan-tools`, `vulkan-validation-layers`, the proprietary NVIDIA Vulkan
driver (including `nvidia-utils` and `nvidia-smi`), `python-numpy`,
`python-pillow` and the same X11 tools (`xorg-xwayland`, `xorg-xrandr`,
`xorg-xrdb`, `xorg-xprop`). Both workflows check these before building.
Check with `nvidia-smi` and `vulkaninfo --summary` before dispatching;
the NVIDIA driver and Vulkan device must support Vulkan 1.4 or newer.
The workflow looks up the NVIDIA ICD manifest, restricts Vulkan device
discovery to it, and verifies an accessible NVIDIA DRM render node. This
avoids accidentally treating the Intel iGPU of a hybrid laptop as tested.

It builds the chosen trusted revision, runs the same functional and
sharpness GPU-labeled CTest entries as the AMD workflow, rejects missing
or skipped suites, and uploads NVIDIA driver details, Vulkan details,
CTest diagnostics and JUnit XML. It does not run `sudo` or install packages.
Its headless Wayland tests do **not** exercise physical display/KMS paths.

To run it after the workflow is merged into `main`, use
Actions → GPU CI - NVIDIA (manual) → Run workflow, selecting a **trusted**
branch. As with AMD, do not dispatch unreviewed pull request code to a
persistent personal machine: the build and tests execute locally. Use
a restricted unprivileged runner account without personal credentials,
and keep the laptop awake during a run.

### Running only some tests

The whole GPU suite takes about 17 minutes on the NVIDIA laptop; checking
one failure again doesn't need it. Both workflows take two optional inputs:

- **tests**: a CTest regex, such as `functional-shell` or
  `functional-(capture|x11-native)`.
- **test_filter**: with **tests**, only the Python test methods whose
  names contain this, like `unittest -k` (`snipping`). Locally:
  `VELA_TEST_FILTER=snipping ctest --test-dir build -R functional-shell`.

A partial run still fails on skipped tests or on a filter that selects
nothing (locally only with `VELA_GPU_CI_STRICT=1`, as in the workflows),
and on a **test_filter** whose **tests** pick no `functional-*` entry, but
doesn't require the full set of suites. With
**Collect coverage** it builds and tests instrumented, as for a coverage
failure, without generating or publishing a (partial) report.

## First-party CPU/GPU code coverage

The [coverage guide](code-quality-integrations.md#codecov-first-party-cc-coverage-cpu-and-optional-gpus)
describes GCC line coverage filtered to production C/C++ sources and
subsystem summaries. The existing GPU test workflows have an optional
**Collect coverage** dispatch input (off by default). When enabled, their
instrumented builds collect real AMD/NVIDIA execution hits only if the
full GPU suite passes with no skips. The report is transferred as a GitHub
artifact to a separate GitHub-hosted Codecov uploader, never uploaded
with extra privileges from a self-hosted runner. Install `gcovr` and GCC
on the runners before opting in; ordinary manual GPU tests are unchanged.
Codecov GPU flags are tied to the sampled commit SHA and not carried forward.

## Prerequisites

GitHub Actions must be enabled. The normal automatically triggered CI needs
no self-hosted runners or secrets; the optional manual AMD and NVIDIA GPU
workflows require their respective explicitly labeled local runners.
Required check / branch protection policy is a separate optional admin setting.
