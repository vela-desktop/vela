# Contributing to Vela

Vela is an alpha Linux desktop environment with a C17 Wayland compositor, Vulkan renderer and Qt 6 user interface. Bug reports, reproducible tests, documentation fixes and code contributions are welcome.

## Before writing code

- Search existing issues and pull requests. For large changes, discuss the intended behavior and architecture in an issue first.
- Keep pull requests focused. Prefer fixing a regression to adding unrelated features in the same change.
- Describe how the change affects AMD/RADV, NVIDIA, Intel, headless and nested sessions when relevant. A CPU-only CI pass does **not** prove a GPU/driver-specific fix.
- Code, commit messages and UI text should be in English; existing source comments may use Italian. Follow the surrounding style.

## Build and test

See [the source build instructions](README.md#build-from-source) and [development guide](docs/development.md).

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The functional and sharpness suites need a usable Vulkan 1.4 GPU and render node. On GitHub-hosted CI they can be marked **skipped**, which is not equivalent to a successful hardware test. For renderer or shell changes, run the relevant suites on real hardware if possible and report the GPU and driver used.

For a safe visual smoke test inside your current Wayland session:

```sh
sh scripts/run-nested.sh
```

Details: [testing](docs/testing.md), [configuration and diagnostics](docs/configuration.md), [known hardware coverage](docs/hardware.md).

## Pull request checklist

- Explain what changed, why it changed, and how you tested it.
- Include precise reproduction steps or before/after evidence for bug fixes.
- Update documentation when dependencies, keyboard shortcuts, behavior or protocols change.
- Run tests appropriate to the modified subsystem. Distinguish **passed**, **skipped**, **not run** and **not applicable**.
- Do not include credentials, unreviewed diagnostic archives, personal paths or private logs in a PR.

## Developer Certificate of Origin

Each commit needs a sign-off using the [Developer Certificate of Origin](https://developercertificate.org/):

```sh
git commit -s -m "Describe the change"
```

The sign-off certifies you have the right to contribute the work under the project's GPL-3.0-or-later license; it is distinct from GPG signing.

## Security-sensitive fixes

Do not publish exploitation details for a new vulnerability in an issue or PR before coordinating disclosure. See [SECURITY.md](SECURITY.md).
