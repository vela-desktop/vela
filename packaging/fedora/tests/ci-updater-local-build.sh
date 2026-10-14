#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run only as root in a disposable Fedora 44 CI container, BEFORE installing
# the real vela-git RPM. Exercises the updater with real Git, DNF5 and RPM.
set -euo pipefail

test "$(id -u)" -eq 0
work=$(mktemp -d)
cleanup() {
    dnf remove -y vela-git >/dev/null 2>&1 || true
    rm -rf -- "$work"
}
trap cleanup EXIT
export XDG_CACHE_HOME="$work/cache" XDG_CONFIG_HOME="$work/config"
mkdir -p "$work/source/packaging/fedora" "$work/bin"

# The CI container is already root: make sudo delegate, while ALL DNF and RPM
# operations still run for real. This isolates the test from sudoers policy.
printf '#!/bin/sh\nexec "$@"\n' > "$work/bin/sudo"
chmod +x "$work/bin/sudo"
export PATH="$work/bin:$PATH"

cat > "$work/source/CMakeLists.txt" <<'CMAKE'
project(vela
    VERSION 0.1.0
)
CMAKE
cat > "$work/source/packaging/fedora/ci-marker" <<'MARKER'
#!/bin/sh
echo vela-fedora-updater-ci
MARKER
chmod +x "$work/source/packaging/fedora/ci-marker"
cat > "$work/source/packaging/fedora/vela.spec" <<'SPEC'
%{!?vela_version:%{error:Missing vela_version}}
%{!?vela_short:%global vela_short 0000000}
%{!?vela_count:%global vela_count 0}
Name:           vela-git
Version:        %{vela_version}
Release:        1.%{vela_count}.g%{vela_short}%{?dist}
Summary:        Disposable Fedora updater integration test package
License:        MIT
Source0:        vela-%{version}.tar.gz
BuildRequires:  bash

%description
Disposable Vela RPM fixture, never shipped to users.

%prep
%autosetup -n vela-%{version}

%build
:

%install
install -Dm755 packaging/fedora/ci-marker %{buildroot}%{_bindir}/vela-ci-marker

%files
%{_bindir}/vela-ci-marker
SPEC
git -C "$work/source" init -q -b main
git -C "$work/source" config user.name "Vela CI"
git -C "$work/source" config user.email "vela-ci@example.invalid"
git -C "$work/source" add .
git -C "$work/source" commit -qm 'Create isolated RPM fixture'
sha=$(git -C "$work/source" rev-parse HEAD)

# Use the actual installed script from this commit, not a mocked runner.
sh packaging/vela-update --source "$work/source" --commit "$sha" --build --jobs 1
test "$(/usr/bin/vela-ci-marker)" = 'vela-fedora-updater-ci'
rpm -q --qf '%{VERSION}-%{RELEASE}\n' vela-git | grep -Eq "^0[.]1[.]0-1[.]1[.]g${sha:0:7}[.]fc44$"
test "$(find "$XDG_CACHE_HOME/vela-update/packages" -maxdepth 1 -name 'vela-git-*.x86_64.rpm' | wc -l)" -eq 1

# An identical selected revision must be a no-op without --force.
if sh packaging/vela-update --check --source "$work/source" --commit "$sha"; then
    echo "Expected --check to report the fixture is already installed" >&2
    exit 1
fi

# Force should exercise the DNF5 reinstall path with the exact same NEVRA.
sh packaging/vela-update --force --build --source "$work/source" --commit "$sha"
test "$(/usr/bin/vela-ci-marker)" = 'vela-fedora-updater-ci'
echo ":: Fedora updater full local-build and force-reinstall integration passed"
