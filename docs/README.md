# Vela documentation

Every document in this directory, grouped by what it is for. The
[project README](../README.md) links only the most important ones.

## Using Vela

- [Features](features.md): what Vela does today, area by area.
- [Installation and updates](installation.md): prerequisites, channels, updates and rollback.
- [Keyboard shortcuts](shortcuts.md).
- [Configuration](configuration.md): the files and environment variables behind Settings, and debug overrides.
- [Hardware coverage and known limitations](hardware.md): CPU and GPU baseline, tested hardware, manual smoke tests.
- [Virtual machines](virtual-machines.md): QEMU/KVM with virtio-gpu and Venus.
- [Fedora 44 VM with Venus](vm-fedora44-venus.md): a step-by-step setup, from host packages to a Vela login.
- [Vela Report](reporting.md): the diagnostic report users can share, and its privacy boundaries.
- [Roadmap](roadmap.md): where Vela is, and what comes next.

## Releases and distribution

- [Release channels](release-channels.md): Beta and Experimental, and the maintainer release process.
- [Release automation](release-automation.md): rehearsal tags and the `release.py` commands for Beta releases.
- [Package provenance](package-provenance.md): how packages are built, signed and verified.
- [Distribution metrics](distribution-metrics.md): the GitHub traffic and download statistics collected, with no runtime telemetry.

## Design

Each design document opens with a status header (see [below](#status-headers)).

- **Compositor and rendering**
  - [Renderer](renderer.md): Vela's own scene and Vulkan renderer, frame timing, effects and title bar.
  - [Visual coherence](visual-coherence.md): one look and one motion across the compositor, the shell and Vela's apps.
  - [Theme system](theme-system.md): themes, styles and their contract.
  - [Taskbar](taskbar.md): which windows a button counts, attention, Super+1…9, the tray's drawer, drag and drop, badges, the taskbar on any edge.
  - [The C core](c-core.md): the compositor's rewrite from C++ to C and the rules the C code follows.
  - [Host allocation policy](memory-allocation-policy.md): how C code obtains memory and fails when it cannot.
  - [Compositor security model](security-model.md): trust boundaries, privileged protocols and the lock across crashes.
- **Desktop services**
  - [Audio](audio.md): PipeWire and WirePlumber, `vela-audio`, the shell and Settings.
  - [Bluetooth](bluetooth.md): `vela-bluetooth`, its agent and the UI.
  - [Brightness](brightness.md): DDC/CI monitors and backlights.
  - [Battery notifications](battery-notifications.md): peripherals and the PC.
  - [Screen sharing](screen-sharing.md): the indicator and Do not disturb while sharing.
  - [Authorization prompt](polkit-agent.md): Vela's polkit agent and dialog.
  - [Updates](updates.md): the update daemon, Settings page and notifications.
  - [Keyboard](keyboard.md): the Keyboard settings, the layout picker and its preview, key behavior, Num Lock and Caps Lock, custom shortcuts, the terminal.

## Change notes

Notes written for a single change: the problem, the fix and how it was
validated. They are kept as a record; the design documents above describe
the current behavior.

- [Vulkan device loss](vulkan-device-loss.md): the device-loss and fatal-render policy (#147).
- [Lock wallpaper quality](lock-wallpaper-quality.md): the password screen's wallpaper blur (#149).
- [Deferred SHM capture readback](capture-readback-async.md): capture without blocking presentation (#132).
- [Lower-cost screen capture](capture-cost.md): which capture consumers need CPU pixels, and the next steps.
- [Shell graphics memory](shell-graphics-memory.md): where the shell's VRAM goes, and hidden panels giving it back.

## Developing

- [Development guide](development.md): building, nested and headless sessions, repository layout, tools.
- [Testing](testing.md): the test strategy and how to run each suite.
- [Performance methodology](performance.md): the scope of published numbers and how to reproduce them.
- [Scene comparison benchmark](scene-diff-benchmark.md): the CPU benchmark of the scene diff.
- [CI quality checks](ci-quality.md): sanitizers, static analysis, coverage and GPU CI.
- [Code quality integrations](code-quality-integrations.md): labeler, Codecov, SonarQube Cloud and DCO.

The repository root also has the [contribution guide](../CONTRIBUTING.md), the
[security policy](../SECURITY.md) and the [contributors](../CONTRIBUTORS.md).

## Status headers

Design documents and change notes open with the same header:

```markdown
> **Status:** Implemented. **Checked:** 17 November 2026, `d784004`.
>
> **Remaining:** what is still open, in a sentence or two, with a link to
> the section that tracks it.
```

**Status** is one of:

| Status | Meaning |
|---|---|
| **Proposed** | Written for discussion; not approved. |
| **Accepted** | The design is approved and implementation is in progress; the document says which parts are done. |
| **Implemented** | On `main`. Validation still open, such as tests on real hardware, goes under *Remaining*. |
| **Superseded** | Replaced; the header links the replacement. |

A change note's status may also name the pull request that merged it.

**Checked** is the date and the `main` commit at which someone confirmed the
status and *Remaining* against the code. It does not certify every claim in
the body. Update it whenever you change the status, or when you check it
again.

Progress per stage or pull request belongs in the document's own stages
section, not in the header.

## Repository policy

Everything under `docs/` is treated as **documentation-only** by the path-aware pull request branch-freshness check. Changes here do not, by themselves, require an open pull request to update its branch from `main`.

Keep executable code, build inputs, CI configuration, generated files consumed by the build or tests, and other runtime-affecting resources **outside** `docs/`. Even if a file looks like documentation, it must not live here if changing it could affect how Vela builds, tests, installs, or runs.

If that assumption ever changes, update the freshness-check policy before introducing the dependency. This exemption only relaxes the branch-update requirement; normal reviews and other required checks still apply.
