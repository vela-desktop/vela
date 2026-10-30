# Installing and updating Vela

Vela is experimental software. The `vela` package is the numbered Beta
channel; `vela-git` tracks Experimental development. Keep a working desktop or TTY available, and review the [hardware coverage](hardware.md) before using a full session.

Vela requires a Vulkan 1.4 GPU and a working PipeWire/WirePlumber audio stack. The commands below install native development snapshots, not stable distribution releases.

## Arch Linux, CachyOS, EndeavourOS

For Experimental, Vela installs as a native `vela-git` package. To install
numbered Beta instead, run `sh vela/packaging/vela-update --channel beta`
after cloning the checkout and installing Cosign. Install `git`, `curl`, `base-devel` and `cosign` (for signed prebuilt packages; GitHub CLI is not required):

```sh
sudo pacman -S --needed git curl base-devel cosign
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update
```

**`vela-update` also handles the first installation**; you do not need to build Vela
or install a package separately. On first run it reuses Git objects from the
checkout instead of downloading the full repository twice. It checks for a
public, commit-matched alpha package built on `main` (no GitHub login required)
and verifies its SHA-256 checksum **and signed build provenance** before installing with pacman. If there is no
matching public package, it can use a temporary CI artifact through an
authenticated [GitHub CLI](https://cli.github.com/) or build the native package
locally. Historical public alpha packages were published after successful `main` builds;
new commits only produce temporary CI artifacts;
experimental branches normally build locally unless you have access to a matching
CI artifact.

Once installed, use **`vela-update`** to fetch new commits and install updates.

## Fedora 44

Fedora 44 x86_64 has an Experimental `vela-git` RPM and a numbered Beta
`vela` RPM. For Beta, run `sh vela/packaging/vela-update --channel beta`
after installing Cosign as described below. RPMs are not yet supported on
other Fedora releases or other DNF-based distributions. The same updater installs it
and handles subsequent updates through DNF. To get started:

```sh
sudo dnf install git curl rpm-build dnf5-plugins
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update
```

**Fedora 44 verification prerequisite:** Fedora 44 does not currently ship
`cosign` in its default repositories. Install the official upstream Cosign RPM
before expecting the updater to use precompiled RPMs:

```sh
version=$(curl -fsSL https://api.github.com/repos/sigstore/cosign/releases/latest |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"].removeprefix("v"))')
curl -fL -o cosign.rpm "https://github.com/sigstore/cosign/releases/download/v${version}/cosign-${version}-1.x86_64.rpm"
sudo dnf install ./cosign.rpm
rm cosign.rpm
```

Only obtain Cosign from the official Sigstore release. This HTTPS bootstrap
assumes the upstream Cosign distribution is trustworthy; it is not independently
authenticated by Vela. If Cosign is missing or verification fails, Vela builds
the selected source commit locally instead of trusting an unsigned binary.
No `gh` installation or GitHub login is needed for public releases.

The updater looks for an RPM built for the exact selected commit in the public
alpha release and verifies both its checksum and signed GitHub build provenance.
If no trusted RPM exists, it builds one from that commit with `rpmbuild` and
installs it with DNF. On Fedora, `--build` also installs build dependencies with
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

Once installed, use **`vela-update`** on either distribution to fetch new
commits and install updates.

## Ubuntu 26.10 (experimental native DEB updater)

Ubuntu **26.10** has an experimental **locally built `.deb` updater**. Enable
the **universe** repository first. Other Ubuntu releases are not supported.
Keep a working desktop or TTY available.

Bootstrap from a trusted Vela checkout:

```sh
sudo apt update
sudo apt install git ca-certificates sudo
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update
```

The updater installs the Ubuntu build dependencies through APT, compiles the
exact selected Git revision with CMake/CPack, and installs a native DEB.
The DEB contains `/usr/bin/vela-update` for subsequent updates. Run
`vela-update --check` to inspect pending changes, `vela-update --force` to
rebuild and reinstall the default branch, or `vela-update --commit SHA` to
downgrade. Use `vela-update --force --commit SHA` to reinstall a specific commit.
Commits older than the revision-stamping support cannot be installed
through this updater unless they produce a correctly stamped DEB.

**On Ubuntu, automatic download of CI preview binaries is intentionally
disabled.** Even `vela-update --download` builds locally. Those artifacts
currently lack the release provenance used by Arch/Fedora; do not
automatically trust or install them. `--build` explicitly opts into the
same local build path.

The last three locally built DEBs remain in
`~/.cache/vela-update/packages/` for manual rollback. Select **one exact**
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

## Updating, selecting revisions, and rollback

Repository downloads use public HTTPS and need no GitHub account or SSH keys.
Saved GitHub SSH URLs and cached mirrors from older versions are migrated to
HTTPS automatically. `--source` can still select a local Git copy.

```sh
vela-update                 # update if something new is available
vela-update --check         # only check, and list the changes
vela-update --download      # download a matching public package, or build if unavailable
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
Public `main` packages are also published as per-commit GitHub prereleases
(tagged `alpha-<full-commit-sha>`) and can be downloaded without a GitHub account.
Only the **five most recently published Vela alpha releases** are kept; older
ones and their tags are removed automatically. For an older commit, `vela-update`
falls back to an available workflow artifact or a local build.
The tester needs a version of `vela-update` that already supports these options.

The last three packages stay in `~/.cache/vela-update/packages/` for rollback.
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
