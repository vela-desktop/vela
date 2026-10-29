#!/usr/bin/env bash
# Real DNF5 local-RPM transaction regression test; run only in a disposable CI container.
set -euo pipefail
work=$(mktemp -d)
cleanup() {
    dnf remove -y vela-dnf-smoke vela-channel-ci vela-channel-ci-git >/dev/null 2>&1 || true
    rm -rf -- "$work"
}
trap cleanup EXIT
mkdir -p "$work"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS}
build_version() {
    local revision="$1"
    cat > "$work/SPECS/vela-dnf-smoke.spec" <<EOF
Name:           vela-dnf-smoke
Version:        1.0.0
Release:        ${revision}%{?dist}
Summary:        Temporary DNF transaction integration fixture
License:        MIT
BuildArch:      noarch

%description
Test RPM used only in the isolated Fedora smoke CI job.

%install
mkdir -p %{buildroot}%{_datadir}/vela-dnf-smoke
echo "$revision" > %{buildroot}%{_datadir}/vela-dnf-smoke/revision

%files
%{_datadir}/vela-dnf-smoke/revision
EOF
    rpmbuild -bb "$work/SPECS/vela-dnf-smoke.spec" --define "_topdir $work" >/dev/null
    cp "$work"/RPMS/noarch/vela-dnf-smoke-1.0.0-"${revision}"*.noarch.rpm "$work/revision-${revision}.rpm"
}
assert_revision() {
    local expected="$1"
    test "$(cat /usr/share/vela-dnf-smoke/revision)" = "$expected"
    rpm -q vela-dnf-smoke >/dev/null
}
build_version 1
build_version 2
dnf install -y "$work/revision-1.rpm"
assert_revision 1
dnf install -y "$work/revision-2.rpm"
assert_revision 2
dnf install -y "$work/revision-1.rpm"
assert_revision 1
dnf reinstall -y "$work/revision-1.rpm"
assert_revision 1
echo ":: Fedora DNF local RPM install, upgrade, downgrade and reinstall passed"


# Validate one-transaction replacements in both directions with disposable
# native RPM packages. Their Conflicts/Obsoletes mirror the actual channel
# contract, without removing the real desktop package from the CI container.
build_channel() {
    local pkg="$1" other="$2"
    cat > "$work/SPECS/$pkg.spec" <<EOF
Name:           $pkg
Version:        0.1.0
Release:        1%{?dist}
Summary:        Disposable Vela release-channel transaction fixture
License:        MIT
BuildArch:      noarch
Conflicts:      $other
$(if [ "$pkg" = vela-channel-ci ]; then echo "Obsoletes:      $other < 1.0.0"; fi)

%description
Disposable migration fixture, never shipped.

%install
mkdir -p %{buildroot}%{_datadir}/vela-channel-ci
echo "$pkg" > %{buildroot}%{_datadir}/vela-channel-ci/marker

%files
%{_datadir}/vela-channel-ci/marker
EOF
    rpmbuild -bb "$work/SPECS/$pkg.spec" --define "_topdir $work" >/dev/null
    cp "$work"/RPMS/noarch/"$pkg"-0.1.0-1*.noarch.rpm "$work/$pkg.rpm"
}
build_channel vela-channel-ci-git vela-channel-ci
build_channel vela-channel-ci vela-channel-ci-git
dnf install -y "$work/vela-channel-ci-git.rpm"
rpm -q vela-channel-ci-git
dnf install -y --allowerasing "$work/vela-channel-ci.rpm"
rpm -q vela-channel-ci
if rpm -q vela-channel-ci-git >/dev/null 2>&1; then
    echo "DNF did not replace the Experimental channel package" >&2; exit 1
fi
dnf install -y --allowerasing "$work/vela-channel-ci-git.rpm"
rpm -q vela-channel-ci-git
if rpm -q vela-channel-ci >/dev/null 2>&1; then
    echo "DNF did not replace the Beta channel package" >&2; exit 1
fi
echo ":: Fedora DNF bidirectional channel replacement passed"
