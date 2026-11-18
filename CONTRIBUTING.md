# Contributing to Vela

Vela is a beta Linux desktop environment with a C17 Wayland compositor, Vulkan renderer and Qt 6 user interface. Bug reports, reproducible tests, documentation fixes and code contributions are welcome.

## Before writing code

- Search existing issues and pull requests. For large changes, discuss the intended behavior and architecture in an issue first.
- Keep pull requests focused. Prefer fixing a regression to adding unrelated features in the same change.
- Open pull requests against `main`, fixes included: maintainers port fixes to the current `release/0.x` branch. Only a fix for code that `main` has since changed targets `release/0.x` directly ([release branches](docs/release-channels.md#release-branches)).
- Describe how the change affects AMD/RADV, NVIDIA, Intel, headless and nested sessions when relevant. A CPU-only CI pass does **not** prove a GPU/driver-specific fix.
- Use English for code, comments, log messages, commit messages and UI text (UI text is translated through Qt Linguist; see `*/i18n/`). Follow the surrounding style.

## Build and test

See [the source build instructions](docs/development.md#build) and the rest of the [development guide](docs/development.md).

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

C and C++ follow `.clang-format` (C directories write `type *name`, C++ `Type& name`). Format your changed lines before pushing. Install clang-format and git-clang-format (CI uses version 18), then run the formatter script from the repository root.

```sh
git fetch origin main
bash scripts/format-changed.sh origin/main
```

For a PR targeting a release branch, fetch that branch and pass its remote-tracking ref (for example, `origin/release/0.x`). The script uses the same exclusions as CI. Add `--diff` to preview the changes. The build job publishes a report of `qmllint` warnings without blocking on lint findings.

The functional and sharpness suites need a usable Vulkan 1.4 GPU and render node. On GitHub-hosted CI they can be marked **skipped**, which is not equivalent to a successful hardware test. For renderer or shell changes, run the relevant suites on real hardware if possible and report the GPU and driver used.

For a safe visual smoke test inside your current Wayland session:

```sh
sh scripts/run-nested.sh
```

Details: [testing](docs/testing.md), [configuration and diagnostics](docs/configuration.md), [known hardware coverage](docs/hardware.md).

Code quality integrations (coverage, automated area labels, SonarQube Cloud, DCO) are described in [docs/code-quality-integrations.md](docs/code-quality-integrations.md).

Lint all Python sources from the repository root before opening a PR:

```sh
uvx ruff==0.17.5 check .
```

Alternatively, install `ruff==0.17.5` with pip and run `ruff check .`.
The root [`ruff.toml`](ruff.toml) enables `E4`, `E7`, `E9`, `F` and pyupgrade
(`UP`) rules for Python 3.13+, including tests and the Python entry points
`packaging/vela-update-state` and `report/vela-report.in`. Generated build
trees and downloaded Arch packaging sources are excluded. The **Python checks**
workflow runs the same check for every pull request and pushes to `main` and
release branches and runs the CPU Python suites on the minimum supported
Python 3.13. When upgrading Ruff, update the version in the configuration,
workflow and commands here together.

## Pull request checklist

- Explain what changed, why it changed, and how you tested it.
- Include precise reproduction steps or before/after evidence for bug fixes.
- Update documentation when dependencies, keyboard shortcuts, behavior or protocols change.
- Run tests appropriate to the modified subsystem. Distinguish **passed**, **skipped**, **not run** and **not applicable**.
- Do not include credentials, unreviewed diagnostic archives, personal paths or private logs in a PR.

## Developer Certificate of Origin

Each commit needs a sign-off using the [Developer Certificate of Origin](https://developercertificate.org/). The DCO GitHub App checks all non-exempt commits once the repository owner enables it:

```sh
git commit -s -m "Describe the change"
```

The sign-off certifies you have the right to contribute the work under the project's GPL-3.0-or-later license; it is distinct from GPG signing.

## Security-sensitive fixes

Do not publish exploitation details for a new vulnerability in an issue or PR before coordinating disclosure. See [SECURITY.md](SECURITY.md).
