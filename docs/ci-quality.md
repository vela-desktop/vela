# Additional CI quality checks

The main Build workflow runs on pushes to `main`, pull requests and manual dispatch; it also invokes the Fedora build and test workflow to gate public releases.
Sanitizer Tests and Static Analysis run separately on pull requests and manual dispatch;
Standalone Package Test is now manual-only because Build already performs the
same Arch package build and smoke test on PRs. Documentation-only PRs skip the
expensive Build jobs after an inexpensive source-change check.

## Package and updater contracts

The [Build workflow](../.github/workflows/build.yml) runs the offline
`packaging/tests` suite on Python 3.13 alongside the Arch and Fedora package
builds. Beta publication requires the suite and the clean-install smoke tests
to pass. The hosted distro CTest jobs exclude `packaging-update` because Build
owns the suite. It checks public HTTPS defaults, migration of old
SSH configuration and mirrors, revision selection, CI artifact selection and
failure handling. It uses real Git with isolated configuration and rejects
unexpected transports; package tools are simulated. See [testing.md](testing.md)
for the local command and test isolation details.

Fedora's test jobs exercise real DNF transactions with fixture RPMs alongside
the native package builds. The Fedora smoke jobs install the generated RPM in
a fresh container and check its runtime dependencies and files.

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

All three manual GPU workflows use pinned Node.js 24-based GitHub Actions and require a
**GitHub Actions self-hosted runner >= 2.327.1**. Update the runner before
dispatching a workflow. Their hardware test jobs set
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
tests: passing them does not certify direct KMS or real display power cycling. See [the physical hardware matrix](hardware.md#gpu-resource-cleanup-and-driver-regression-checks).

## GPU CI policy

This is the **normative source** for when real-hardware GPU CI must run.
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
  missing, failed or skipped test cases. Qt Quick's separate OpenGL
  `text-rendering-rhi` test belongs to hosted Mesa/Xvfb CI, not to the
  real-hardware compositor GPU suite, and is still mandatory there.
  `vulkaninfo` by itself is not a pass; a headless GPU pass does not certify
  physical DRM/KMS, VT
  switching, screen sharing, or suspend/resume.
- **Runner safety:** only manually dispatch trusted, reviewed code. AMD and
  Intel use persistent personal self-hosted machines: keep those runner accounts
  unprivileged and isolated from personal files, privileged sudo and writable
  secrets. NVIDIA uses a disposable Vast.ai rental; the GitHub App and Vast.ai
  credentials stay in the GitHub-hosted orchestration jobs.

The two mandatory hardware test paths are the self-hosted AMD Radeon
RX 9070 XT (Mesa/RADV, CachyOS) and an NVIDIA RTX GPU rented per run on
Vast.ai (proprietary driver, Arch Linux container). The NVIDIA GPU model can
vary between runs. An experimental Intel Alder Lake UHD Graphics runner
(Mesa/ANV, CachyOS) adds optional coverage; it is not yet required for
merges or releases.

**Enforcement today:** this is a maintainer/release policy, **not yet an
automatic GitHub branch-protection gate**. All three workflows currently use
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
`python-dbus`, `ccache` and the X11 tools of the native-resolution tests (`xorg-xwayland`,
`xorg-xrandr`, `xorg-xrdb`, `xorg-xprop`).
It must be able to access an AMD DRM render node and the RADV Vulkan 1.4
driver. Install dependencies **on the host**, not inside the workflow;
the workflow deliberately runs without `sudo` or host package upgrades.
The build uses `ccache` in the runner user's persistent home cache, while
each run still configures and builds the exact selected revision from scratch.
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

## Manual real-GPU CI (rented NVIDIA on Vast.ai)

[GPU CI - NVIDIA (manual)](../.github/workflows/gpu-nvidia.yml) is
triggered only via `workflow_dispatch`. It does **not** run on the
former RTX 4070 laptop runner. A GitHub-hosted orchestration job uses
[scripts/vast-runner.py](../scripts/vast-runner.py) to rent a verified
single-GPU NVIDIA RTX instance on Vast.ai, register a uniquely labeled
just-in-time GitHub Actions runner, and launch the hardware job in an
ephemeral Arch Linux container. The instance and runner registration
are destroyed in the cleanup job, including when the GPU job fails.

The `gpu` input optionally selects a supported RTX model; when left
empty, the script ranks fitting offers by hourly price, effective CPU
capacity, reported host reliability, and NVIDIA driver version, subject to
the configured price cap (currently $0.12/hour). It is a cost/performance
heuristic, not a historical benchmarking service. Offers must meet the
script's CPU, RAM, disk, GPU VRAM, reliability and driver requirements.
The actual model and driver can vary between runs. During provisioning,
an image that cannot start within six minutes is rejected; once the
instance reports running, the JIT runner must register within 150 seconds.
Known fatal image-pull errors (such as DNS failures) are rejected earlier,
and the next eligible rental is tried. The GPU's proprietary
driver, Vulkan ICD and DRM render nodes are provided by the rental host;
build and test dependencies come from the
[NVIDIA CI image](../.github/images/nvidia-ci/Dockerfile). The
[image publishing workflow](../.github/workflows/nvidia-ci-image.yml)
must first publish the image matching the selected revision's image tree.
Before renting, the orchestration job resolves that tag to an immutable OCI
SHA256 manifest digest; the rented host pulls that exact image. Tree tags
identify source configuration, not dependency versions: a scheduled weekly
rebuild intentionally refreshes Arch packages even if the Dockerfile has not
changed. Each publication also receives a unique run tag, and `latest` is
updated only from `main`. The publish job saves package versions, an SPDX
SBOM, an `arch-audit` report and a Trivy report as artifacts, and blocks
publication when either finds a fixable high or critical vulnerability. Trivy
has no Arch Linux driver and cannot see pacman packages, so those are checked
by `arch-audit --upgradable` against the
[Arch security tracker](https://security.archlinux.org/) and
[scripts/ci-arch-audit-gate.py](../scripts/ci-arch-audit-gate.py); Trivy then
covers only packages installed outside pacman. If the tracker cannot be
reached or its report cannot be read, publication fails too. The image leaves out
libcap's `captree`: Arch builds it with Go only when libcap changes, so its Go
standard library falls behind Go's security fixes, and nothing in the CI
uses it.

The image qualification step checks both the mandatory Vulkan/CTest tools
and optional promo recording prerequisites before publication. In particular,
Arch's `pipewire-audio` package supplies `pw-cat` (not the base `pipewire`
package); this was missing in NVIDIA GPU run #131 on 16 November 2026, where
the full GPU CTest suite succeeded but optional promo smoke failed. The
container runner uses `runuser` without a passwordless-sudo rule and checks
the runner archive against the SHA256 published with its GitHub release before
extracting it. These checks do not replace a real NVIDIA hardware run.

The NVIDIA test job sizes Ninja parallelism from the offered effective CPU
allocation, container CPU affinity/quota and available host/cgroup memory,
reserving memory for the system and capping builds at 16 concurrent jobs.
Unlike the AMD and Intel self-hosted runners, no persistent compilation
cache is configured for these one-off rentals.

The test job checks a real NVIDIA DRM render node and Vulkan >= 1.4,
restricts Vulkan discovery to the NVIDIA ICD, builds the exact checkout,
runs the GPU-labeled functional and sharpness suites, rejects missing
or skipped tests and uploads driver/Vulkan/CTest/JUnit diagnostics. The
suite uses a private headless Wayland session, **not** physical DRM/KMS.
The GitHub App key and Vast.ai API key are used only by the GitHub-hosted
orchestration jobs, not exposed to the rented test container. The
hardware job does not install host packages or upgrade the rental host.

Use Actions → GPU CI - NVIDIA (manual) → Run workflow on a **trusted,
reviewed** revision. Running unreviewed code on rented hardware is still
unsafe; do not assume an ephemeral machine makes untrusted scripts safe.
Do not confuse the historical RTX 4070 laptop tests with the current
Vast.ai-backed NVIDIA CI.

## Manual real-GPU CI (self-hosted Intel, experimental)

[GPU CI - Intel (manual)](../.github/workflows/gpu-intel.yml) targets an
organization runner labeled **`vela-intel`** (plus `self-hosted`,
`linux`, `x64`). It is **opt-in and informational**, not an additional
mandatory merge or release check. Like the AMD runner, this workflow only
supports `workflow_dispatch`: PR and push events do not dispatch it.
The organization runner group must permit access to the public
`vela-desktop/vela` repository; restrict its scope to trusted workflows.

Initial test machine: Intel Alder Lake-UP3 UHD Graphics (PCI `8086:46b3`),
Mesa ANV 26.2.5 on CachyOS, 8 GB RAM and 256 GB UFS. Vulkan 1.4,
the expected driver and the necessary DRM/dma-buf extensions have been
verified manually. This does **not** yet establish a passing GPU CI baseline.

Install the [normal build dependencies](development.md#distribution-dependencies)
plus `vulkan-intel`, `vulkan-tools`, `vulkan-validation-layers`,
`python-numpy`, `python-pillow`, `python-dbus`, `dbus`,
`xorg-xwayland`, `xorg-xrandr`, `xorg-xrdb`, `xorg-xprop`, `breeze-icons`,
`ccache` on the host. The workflow never installs packages or uses sudo.
`hicolor-icon-theme` alone supplies no standard action icons. The real-shell
tests run `vela-shell --check-icons` with their private HOME and configuration;
this checks actual Qt rendering in light and dark mode, including SVG loading,
and names missing icons before exercising QML. The shell adds the standard XDG
icon directories even without a KDE platform plugin; that plugin is not a
requirement. Provision a CachyOS/Arch Intel
runner with `sudo pacman -S --needed breeze-icons qt6-svg`, then rerun the failed
shell suite. Installing the packages is a host administration step, not a
workflow action.
It isolates Intel's Mesa ANV ICD (not `intel_hasvk` or Lavapipe),
requires `DRIVER_ID_INTEL_OPEN_SOURCE_MESA`, Vulkan >= 1.4 and
PCI vendor `0x8086`, and verifies the DRM render node actually
selected by Vela. After building, a separate headless compositor smoke
check verifies Vela's actual Intel renderer **even for partial runs**
that select only sharpness or CPU-fence tests. A qualifying `vulkaninfo`
result alone is never reported as an Intel compositor pass.

The Intel build uses up to **eight concurrent compiler jobs** (bounded
by `nproc`) on the 8 GB machine, with a 180-minute job timeout. The
`ccache` compilation cache persists in the runner user's home directory
across workflow runs; monitor memory pressure under the higher job cap.
It executes the same GPU-labeled functional, sharpness and sharpness-cpu suites, retains
strict no-skip/no-missing-test enforcement, shutdown stress and
validation layers, and uploads Intel-specific Vulkan/CTest/JUnit
diagnostics. SHM capture uses a 60 Hz baseline (`VELA_CAPTURE_TEST_REFRESH_HZ`),
raised to 120 Hz for the single-client 1440p stream's 60 fps floor, with the
same fps, latency and missed-slot limits as AMD's 180 Hz workload. This
qualifies the stated refresh rather than claiming 180 Hz performance on the
iGPU. Optional coverage (`gpu-intel`) and promo smoke tests
mirror AMD; the coverage upload is performed by a separate
GitHub-hosted job, not the self-hosted runner.

The first run establishes the baseline. Integrated Intel graphics
previously exposed timing-sensitive `test_ready` failures because
the client and compositor share the GPU engine
([C core notes](c-core.md)); investigate rather than silently
skip or loosen assertions. Headless CI does not certify physical
KMS, suspend/resume or display power cycling.

Run the service as an unprivileged account, without passwordless sudo,
personal credentials or access to personal files, and dispatch only
trusted, reviewed revisions. It can coexist with a Niri desktop:
the workflow builds a temporary copy of Vela and runs it headlessly,
although build/GPU load may affect responsiveness.

### Running only some tests

A full GPU suite can take many minutes; its runtime on rented NVIDIA
hardware varies by host. Rerunning one failure does not require a full
suite. All three workflows accept two optional test-selection inputs:

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
