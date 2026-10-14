#!/usr/bin/env bash
# Real DNF5 local-RPM transaction regression test; run only in a disposable CI container.
set -euo pipefail
work=$(mktemp -d)
cleanup() {
    dnf remove -y vela-dnf-smoke >/dev/null 2>&1 || true
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
