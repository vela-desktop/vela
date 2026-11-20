# Installing and updating Vela

Vela is beta software. Keep a working desktop or TTY available, and review
the [hardware coverage](hardware.md) before using a full session.

Vela requires Python 3.13 or newer, Linux 6.5 or later ([why](security-model.md#privileged-protocols)), a Vulkan 1.4 GPU and a working PipeWire/WirePlumber audio stack.
`vela-update` installs it on Arch, Fedora 44/45 and Ubuntu 26.10 as a native
package, on one of two [channels](release-channels.md):

| Channel | Package | What it installs |
|---|---|---|
| **Beta** (recommended) | `vela` | The newest numbered [release](https://github.com/vela-desktop/vela/releases): a prebuilt package, verified with Cosign against its signed provenance before installing. Never built locally. |
| Experimental | `vela-git` | A development commit (`main`, or a branch or commit you choose), built locally; on Arch and Fedora it can instead use that commit's signed CI package through an authenticated GitHub CLI. |

Run `vela-update --channel beta` for Beta; without `--channel`, the first
installation is Experimental. Afterwards the installed package picks the
channel, and Settings → System → Updates can switch it.

The Beta needs **Cosign** on every distribution: installation stops if it
is missing, rather than trusting an unverified binary.

## Arch Linux, CachyOS, EndeavourOS

Install `git`, `curl`, `base-devel` and `cosign` (GitHub CLI is not required):

```sh
sudo pacman -S --needed git curl base-devel cosign
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update --channel beta
```

**`vela-update` also handles the first installation**; you do not need to build Vela
or install a package separately. Beta downloads the release's `vela` package
and verifies its SHA-256 checksum **and signed build provenance** before
installing it with pacman.

Experimental (`sh vela/packaging/vela-update`) installs `vela-git`. On first
run it reuses Git objects from the checkout instead of downloading the full
repository twice. It can use a temporary, attested CI artifact for the
selected commit through an authenticated [GitHub CLI](https://cli.github.com/),
verified the same way; otherwise it builds the native package locally.

Once installed, use **`vela-update`** to install updates.

## Fedora 44 and 45

These instructions cover DNF-based editions of Fedora 44 and 45 on x86_64.
Each release has its own RPM. Immutable editions such as Silverblue and
Kinoite are outside the installer's scope.

### Install the Beta

Install the tools used below:

```sh
sudo dnf install git curl python3 util-linux
```

The Beta updater uses Cosign to verify Vela's packages. If you already have
a trusted Cosign installation, skip to the next step. Otherwise, install
the x86_64 RPM from Sigstore's official release. This step trusts the RPM
downloaded from Sigstore over HTTPS.

```sh
version=$(curl -fsSL https://api.github.com/repos/sigstore/cosign/releases/latest |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"].removeprefix("v"))')
curl -fL -o cosign.rpm "https://github.com/sigstore/cosign/releases/download/v${version}/cosign-${version}-1.x86_64.rpm"
sudo dnf install ./cosign.rpm
rm cosign.rpm
```

Install Vela from a trusted checkout:

```sh
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update --channel beta
```

The updater verifies the release checksum and signed build provenance before
passing the RPM to DNF. Beta uses public downloads and needs no GitHub account.
After installation, choose **Vela** at the login screen.

### Install Experimental

To build the development version locally, run:

```sh
sudo dnf install git python3 util-linux rpm-build dnf5-plugins
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update --channel experimental --build
```

The updater installs build dependencies with `dnf builddep`, builds the RPM
for your Fedora release and installs it with DNF. To let it try a prebuilt
CI RPM first, omit `--build`. This path needs an authenticated GitHub CLI
and Cosign to download and verify the RPM for the selected commit. If a
verified package is unavailable, the updater builds that commit locally.

### Fedora package details

- Git, DNF5, RPM and `flock` (from util-linux) are runtime dependencies.
  If DNF skips weak dependencies, install `rpm-build` and `dnf5-plugins`
  before a local build. Binary downloads also need `curl` and Cosign.
- Vela verifies Sigstore build provenance. The RPMs carry no OpenPGP package
  signature, so DNF installations that require `localpkg_gpgcheck` will reject
  them. The updater respects that setting.
- CI tests the RPMs in separate Fedora 44 and 45 containers. A Wayland login
  on a physical GPU requires a manual test. See [hardware coverage](hardware.md).

For an existing installation, see [updates and rollback](#updating-selecting-revisions-and-rollback)
or [upgrading from Fedora 44 to 45](#after-upgrading-fedora-44-to-fedora-45).

## Ubuntu 26.10

Ubuntu **26.10** has a Beta `vela` DEB and an Experimental, **locally built**
`vela-git` DEB. Enable the **universe** repository first. Other Ubuntu
releases are not supported, and Ubuntu support is newer and less tested than
Arch and Fedora. Keep a working desktop or TTY available.

For Beta, install Cosign first (Ubuntu 26.10 has it in universe), then
bootstrap from a trusted Vela checkout:

```sh
sudo apt update
sudo apt install git ca-certificates sudo cosign
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update --channel beta
```

Beta downloads the release's DEB, verifies its checksum and signed
provenance, and installs it with APT.

On Beta `v0.1.2`, install the following packages for file choosers in
sandboxed apps, screenshots and screen sharing. The Experimental DEB built
from current `main` recommends all three, so on that channel this step is
only needed if APT skips recommendations.

```sh
sudo apt install xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr
```

Experimental (`sh vela/packaging/vela-update`) installs the Ubuntu build
dependencies through APT, compiles the exact selected Git revision with
CMake/CPack, and installs a native DEB.
The DEB contains `/usr/bin/vela-update` for subsequent updates. Run
`vela-update --check` to inspect pending changes, `vela-update --force` to
rebuild and reinstall the default branch, or `vela-update --commit SHA` to
downgrade. Use `vela-update --force --commit SHA` to reinstall a specific commit.
Commits older than the revision-stamping support cannot be installed
through this updater unless they produce a correctly stamped DEB.

**On Ubuntu, Experimental never downloads CI preview binaries.** Even
`vela-update --download` builds locally. Those artifacts lack the release
provenance used by Arch/Fedora and Beta; do not install them as trusted
packages. `--build` explicitly opts into the
same local build path.

The last three locally built DEBs remain in
`~/.cache/vela-update/packages/` for manual rollback. Build dependencies come
from the selected revision's `packaging/ubuntu/build-depends`, and `apt-get`
runs only when one of them is missing. Select **one exact**
cached filename; never install every cached package with a wildcard:

```sh
sudo apt-get install -y --allow-downgrades --reinstall \
  "$HOME/.cache/vela-update/packages/vela-git_VERSION_amd64.deb"
```

Replace `vela-git_VERSION_amd64.deb` with the actual filename. If a
previous package transaction left dpkg incomplete, first run
`sudo dpkg --configure -a`, then retry the update.

CI also publishes **unsigned experimental preview packages** for manual
testing. Its build includes development tools; updater builds omit them.
See [development.md](development.md#distribution-dependencies) for the
manual source build and prerequisites. CI checks compilation, DEB metadata,
installed entry points, dependency resolution, and removal in a fresh
Ubuntu 26.10 container. **That does not prove a graphical session works.**
Real GPU/DRM, portal, audio, polkit and display-manager testing is still
required. Do not install these packages on other Ubuntu releases.

## Bluetooth

Bluetooth needs only **BlueZ**, with its service running. Vela pairs,
connects and answers devices itself ([Bluetooth](bluetooth.md)): BlueDevil,
blueman or GNOME's Bluetooth panel are not needed.

| Distribution | BlueZ |
|---|---|
| Arch Linux and derivatives | `bluez` is an optional dependency: `sudo pacman -S --needed bluez` |
| Fedora 44/45 | `bluez` is a weak dependency, installed unless weak dependencies are disabled |
| Ubuntu 26.10 | `bluez` is recommended, installed unless recommends are disabled |

Then turn the service on once: `sudo systemctl enable --now bluetooth`.

Vela's daemon, `vela-bluetooth`, starts with the session (in `/usr/lib/vela`
on Arch, `/usr/libexec/vela` on Fedora and Ubuntu). It shares BlueZ with other desktops on
the same machine: while you are in another session, that desktop answers
its devices, and Vela changes nothing.

## Updating from the desktop

**Settings > System > Updates** shows whether an update is available, what's
new (Beta's release notes, or Experimental's commits) and the update channel.
Automatic checks run daily by default, skip metered connections and notify
you when an update is available. On Beta `v0.1.2`, you can turn these checks
on or off. On Experimental, you can also choose an interval of 6, 12, 24 or
48 hours or one week, and see when the next automatic check is due.

**Install now** runs `vela-update` in a terminal, where the package manager
asks for your password. The updater shipped with Experimental also shows the
packaging recipe for the selected commit. If it opens in `less`, press `q`
to continue. Updates started by hand in a terminal show on the page too.
`xdg-terminal-exec` (a package dependency) opens your default terminal,
or Konsole without one. After an update,
sign out and in again to use the new version: the page and a notification say
so. The design is in [Updates](updates.md).

## Updating, selecting revisions, and rollback

Repository downloads use public HTTPS and need no GitHub account or SSH keys.
Saved GitHub SSH URLs and cached mirrors from older versions are migrated to
HTTPS automatically. `--source` can still select a local Git copy.

On Beta, `vela-update` installs the newest release, `vela-update --check`
only checks, `--force` reinstalls it, and `--tag=v0.x.y` goes back to an
older release (a newer installed Beta is never downgraded without it).
The other options are Experimental's:

```sh
vela-update                 # update if something new is available
vela-update --check         # only check, and list the changes
vela-update --download      # download a verified CI package if available, or build
vela-update --build         # always build locally
vela-update --jobs 2        # fewer parallel compile jobs
vela-update --branch YOUR_TEST_BRANCH --download  # replace with an existing branch
vela-update --commit abc1234 --download    # test an exact commit
```

`--branch` and `--commit` apply only to that invocation and cannot be combined.
The next plain `vela-update` returns to the repository's default branch (`main`
on GitHub), including when this means installing an older version. Commit SHAs
may be full or abbreviated, provided the abbreviation is unambiguous.
Both options also work with `--check` and `--build`; local builds use the exact
selected commit too. `--download` uses that commit's CI package if available,
otherwise it builds locally as usual.

To prepare a package for a test branch, run the **Build** workflow manually in
GitHub Actions, selecting that branch (or run
`gh workflow run build.yml --ref YOUR_TEST_BRANCH` for maintainers). Replace `YOUR_TEST_BRANCH` with a real branch name. Manual runs produce both the `vela-git` Arch artifact and the
`vela-fedora-rpm` (44) and `vela-fedora-45-rpm` (45) artifacts, as on pushes to `main`; workflow artifacts expire
after 14 days. PR builds also smoke-test both packages, but their artifacts are
not attested, and `vela-update` does not accept them as trusted downloads.
The GitHub CLI is only needed for optional private workflow-artifact retrieval;
Sigstore/Cosign performs all package verification.
Pushes to `main` do not publish GitHub releases (see
[release channels](release-channels.md)), and the historical per-commit
`alpha-<full-commit-sha>` prereleases have been retired. Experimental
downloads therefore need the GitHub CLI and an unexpired workflow artifact;
otherwise `vela-update` builds locally. Signed packages that need no GitHub
account are the Beta channel's.
The tester needs a version of `vela-update` that already supports these options.

The last three packages of each channel stay in `~/.cache/vela-update/packages/`
for rollback (`vela-git` for Experimental, `vela` for Beta; on Arch, with their
signatures and `-debug` packages when makepkg builds them).
Restore a known-good Arch package with `sudo pacman -U /path/to/vela-git-*.pkg.tar.zst`,
or on Fedora with `sudo dnf install /path/to/vela-git-*.x86_64.rpm`
(select **one exact file**, never install every cached package at once).
For Fedora, `dnf reinstall` handles a same-version forced reinstall.

### After upgrading Fedora 44 to Fedora 45

An updater installed from an older Fedora 44 RPM may reject Fedora 45.
Start from a fresh checkout, then run the command for your installed channel:

```sh
git clone https://github.com/vela-desktop/vela.git vela-fedora-45
```

- **Beta** (`vela`): `sh vela-fedora-45/packaging/vela-update --channel beta`
- **Experimental** (`vela-git`): `sh vela-fedora-45/packaging/vela-update --channel experimental`

For a Beta install or rollback on Fedora 45, choose a release with both a
Fedora 45 RPM and its `SHA256SUMS-fedora-45` manifest. The first is
`v0.1.2`. Releases `v0.1.0` and `v0.1.1` supply Fedora 44 RPMs only.
If the Fedora 45 assets for a release are missing, keep your installed Beta.

## Other distributions

Follow the [source-build instructions](development.md#build), then:

```sh
sudo cmake --install build   # into /usr/local, plus /etc/pam.d and /etc/xdg
```

If the session doesn't show up, your display manager may not look in `/usr/local`:

```sh
sudo ln -s /usr/local/share/wayland-sessions/vela.desktop /usr/share/wayland-sessions/
```

For source-build prerequisites, see the [developer build guide](development.md#distribution-dependencies).
