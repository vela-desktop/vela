# Release history

What each Beta series shipped and how it was validated. The
[changelog](../CHANGELOG.md) lists every user-visible change by version, and the
[roadmap](roadmap.md) covers what comes next.

## 0.1

### The first Beta

Vela [0.1.0](https://github.com/vela-desktop/vela/releases/tag/v0.1.0)
shipped on 31 October 2026 for Arch Linux, Fedora 44 and Ubuntu 26.10.
Users could install a signed native package, choose Vela at login and get
updates from Settings. The Updates page was the one feature admitted during
the freeze. It showed the installed and available versions and the release
notes, then opened a terminal for the installation.
Real updates were tested in clean containers for all three distributions.
See the [Updates design](updates.md) and [#161](https://github.com/vela-desktop/vela/pull/161),
which superseded [#133](https://github.com/vela-desktop/vela/pull/133).

Several changes made the desktop ready for daily use. Vela's Bluetooth
agent handled pairing and PIN or passkey prompts from the shell and Settings.
The GTK and wlr portals supplied file choosers, screenshots and screen
sharing without requiring Plasma Workspace. X11 games could run at native
resolution and use direct scanout, and the lock screen kept its blurred
wallpaper sharp. See the [Bluetooth design](bluetooth.md),
[#146](https://github.com/vela-desktop/vela/pull/146),
[#157](https://github.com/vela-desktop/vela/pull/157),
[portal packaging](https://github.com/vela-desktop/vela/pull/120),
[X11 scaling](https://github.com/vela-desktop/vela/pull/167) and the
[lock-screen change](lock-wallpaper-quality.md) in [#149](https://github.com/vela-desktop/vela/pull/149).

On NVIDIA, deferred shared-memory readback kept screen capture from blocking
presentation while preserving the existing test thresholds.
The compositor also learned to detect Vulkan device loss and exit so the
supervisor could restart it. See [capture issue #132](https://github.com/vela-desktop/vela/issues/132),
[#156](https://github.com/vela-desktop/vela/pull/156),
[#173](https://github.com/vela-desktop/vela/pull/173) and the
[device-loss policy](vulkan-device-loss.md) implemented in [#147](https://github.com/vela-desktop/vela/pull/147).

Before publishing, the project ran the full AMD and NVIDIA suites on the
release commit and tested physical login sessions. A release rehearsal on a
public fork caught two bugs before the official tag. In a real session,
Settings found the Beta, opened the update terminal and prompted the user to
sign out to finish. The two [release channels](release-channels.md) established in 0.1 were Beta for
signed releases and Experimental for development revisions.
See the [GPU test policy](ci-quality.md#gpu-ci-policy),
[rehearsal](release-channels.md#rollout-validation),
[release fixes](https://github.com/vela-desktop/vela/pull/177),
[session checks](updates.md#11-stages) and [channel implementation](https://github.com/vela-desktop/vela/pull/160).

### Maintenance releases

[0.1.1](https://github.com/vela-desktop/vela/releases/tag/v0.1.1) shipped
on 3 November 2026. It addressed everyday input problems, including volume
controls that could stick and window borders that caught an unexpected
resize. It also reduced the size of the Ubuntu package.

[0.1.2](https://github.com/vela-desktop/vela/releases/tag/v0.1.2) shipped
on 16 November 2026 with two exceptions to the maintenance scope: Fedora 45
gained its own RPM alongside Fedora 44, and, to fix
[#222](https://github.com/vela-desktop/vela/issues/222), users could choose
KWallet or GNOME Keyring for app passwords while Vela Secrets was still
pending.
See the [Fedora packaging change](https://github.com/vela-desktop/vela/pull/224)
and the keyring changes in [#255](https://github.com/vela-desktop/vela/pull/255)
and [#277](https://github.com/vela-desktop/vela/pull/277).

Other backports improved allocation recovery, renderer pass reuse, resource
limits and compositor security. They also stabilized [text during animation
and scrolling](https://github.com/vela-desktop/vela/pull/217), prevented
[background apps from stealing focus](https://github.com/vela-desktop/vela/pull/264)
and reduced [copying during screen capture](https://github.com/vela-desktop/vela/pull/260).
Fixes kept [crowded taskbars navigable](https://github.com/vela-desktop/vela/pull/266),
preserved [preferences after a failed save](https://github.com/vela-desktop/vela/pull/267)
and restored [XDG icon theme discovery](https://github.com/vela-desktop/vela/pull/259).
The [0.1.2 tracker](https://github.com/vela-desktop/vela/issues/225) records
the backports.

### Moving to 0.2

When 0.2.0 ships, maintenance and security updates move to 0.2.x and
support for 0.1.x ends. The updater shipped in 0.1.0–0.1.2 discovers only
GitHub prereleases. To reach 0.2.0 from those versions, follow the
[one-time update instructions](release-channels.md#updating-older-beta-installations).
See the [release branch policy](release-channels.md#release-branches) and
the [roadmap](roadmap.md) for the next release.
