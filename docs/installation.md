# Installing and updating Vela

Vela is beta software. Keep a working desktop or TTY available, and review
the [hardware coverage](hardware.md) before using a full session.

Vela requires a Vulkan 1.4 GPU and a working PipeWire/WirePlumber audio stack.
`vela-update` installs it on Arch, Fedora 44 and Ubuntu 26.10 as a native
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

## Fedora 44

Fedora 44 x86_64 has a Beta `vela` RPM and an Experimental `vela-git` RPM.
RPMs are not supported on other Fedora releases or other DNF-based
distributions. The same updater installs Vela and handles subsequent updates
through DNF.

**Fedora 44 verification prerequisite:** Fedora 44 does not currently ship
`cosign` in its default repositories. Install the official upstream Cosign RPM
first; the Beta needs it, and Experimental needs it to use precompiled RPMs:

```sh
version=$(curl -fsSL https://api.github.com/repos/sigstore/cosign/releases/latest |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"].removeprefix("v"))')
curl -fL -o cosign.rpm "https://github.com/sigstore/cosign/releases/download/v${version}/cosign-${version}-1.x86_64.rpm"
sudo dnf install ./cosign.rpm
rm cosign.rpm
```

Only obtain Cosign from the official Sigstore release. This HTTPS bootstrap
assumes the upstream Cosign distribution is trustworthy; it is not independently
authenticated by Vela. Then install Vela:

```sh
sudo dnf install git curl rpm-build dnf5-plugins
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update --channel beta
```

No `gh` installation or GitHub login is needed for Beta releases. If Cosign
is missing or verification fails, Beta stops without installing anything;
Experimental builds the selected source commit locally instead of trusting an
unsigned binary.

For Experimental, the updater can use the signed CI RPM for the exact selected
commit through an authenticated GitHub CLI, and verifies both its checksum and
signed GitHub build provenance. If no trusted RPM exists, it builds one from
that commit with `rpmbuild` and installs it with DNF. On Fedora, `--build` also installs build dependencies with
`dnf builddep`. An existing Arch package is **never** installed on Fedora.
Use `vela-update --check` to inspect changes without installing.

The RPM requires Git, DNF5, RPM and `flock` (from util-linux), even when
installed directly without following the bootstrap commands above. It recommends
`curl` for signed binary downloads, plus `rpm-build` and
`dnf5-plugins` for the local build fallback. If weak dependencies were
disabled during installation, install those optional tools before using their
respective updater modes. Cosign is installed separately (see above).

Fedora RPMs are built and installation-tested in a fresh Fedora 44 container
before release; a physical GPU login and other Fedora versions still require
testing. Fedora's immutable/ostree-based variants are not supported by this
DNF-based installer.

Once installed, use **`vela-update`** on either distribution to install
updates.

## Ubuntu 26.10

Ubuntu **26.10** has a Beta `vela` DEB and an Experimental, **locally built**
`vela-git` DEB. Enable the **universe** repository first. Other Ubuntu
releases are not supported, and Ubuntu support is newer and less tested than
Arch and Fedora. Keep a working desktop or TTY available.

For Beta, install a trusted Cosign executable first, then bootstrap from a
trusted Vela checkout:

```sh
sudo apt update
sudo apt install git ca-certificates sudo
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update --channel beta
```

Beta downloads the release's DEB, verifies its checksum and signed
provenance, and installs it with APT.

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
| Fedora 44 | `bluez` is a weak dependency, installed unless weak dependencies are disabled |
| Ubuntu 26.10 | `bluez` is recommended, installed unless recommends are disabled |

Then turn the service on once: `sudo systemctl enable --now bluetooth`.

Vela's daemon, `vela-bluetooth`, starts with the session (in `/usr/lib/vela`
on Arch, `/usr/libexec/vela` on Fedora and Ubuntu). It shares BlueZ with other desktops on
the same machine: while you are in another session, that desktop answers
its devices, and Vela changes nothing.

## Updating from the desktop

**Settings > System > Updates** shows whether an update is available, what's
new (Beta's release notes, or Experimental's commits) and the update channel.
Vela checks by itself once a day, never on a metered connection, and sends a
notification when there is something new; turn this off on the same page.

**Install now** runs `vela-update` in a terminal, where the package manager
asks for your password and the build stays readable; updates started by hand
in a terminal show on the page too. `xdg-terminal-exec` (a dependency of the
package) opens your default terminal, or Konsole without one. After an update,
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
`vela-fedora-rpm` artifact, as on pushes to `main`; workflow artifacts expire
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
