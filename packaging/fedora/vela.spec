# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fedora 44 x86_64 alpha package. Build exactly the selected Git commit:
#   git archive --format=tar.gz --prefix=vela-0.1.0/ HEAD > SOURCES/vela-0.1.0.tar.gz
#   rpmbuild -bb packaging/fedora/vela.spec --define "_topdir <rpmbuild-dir>" \
#     --define "vela_version <CMake project version>" \
#     --define "vela_commit <full-sha>" --define "vela_short <short-sha>" \
#     --define "vela_count <git-rev-list-count>"
#
# The release includes its Git revision to match vela-update's Arch behavior.
%{!?vela_version:%{error:Missing vela_version (read VERSION from CMakeLists.txt)}}
%{!?vela_commit:%global vela_commit unknown}
%{!?vela_short:%global vela_short 0000000}
%{!?vela_count:%global vela_count 0}

Name:           vela-git
Version:        %{vela_version}
Release:        1.%{vela_count}.g%{vela_short}%{?dist}
Summary:        Modern Wayland desktop environment with its own Vulkan renderer
License:        GPL-3.0-or-later
URL:            https://github.com/vela-desktop/vela
Source0:        vela-%{version}.tar.gz
ExclusiveArch:  x86_64

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconf-pkg-config
BuildRequires:  glslc
BuildRequires:  vulkan-headers
BuildRequires:  vulkan-loader-devel
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  wlroots-devel >= 0.20
BuildRequires:  libxkbcommon-devel
BuildRequires:  pixman-devel
BuildRequires:  libinput-devel
BuildRequires:  libdrm-devel
BuildRequires:  mesa-libgbm-devel
BuildRequires:  freetype-devel
BuildRequires:  harfbuzz-devel
BuildRequires:  fontconfig-devel
BuildRequires:  librsvg2-devel
BuildRequires:  cairo-devel
BuildRequires:  zlib-devel
BuildRequires:  pam-devel
BuildRequires:  polkit-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtbase-private-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtsvg-devel
BuildRequires:  qt6-qtwayland-devel
BuildRequires:  qt6-qttools-devel
BuildRequires:  qt6-qtmultimedia-devel
BuildRequires:  layer-shell-qt-devel
BuildRequires:  python3
BuildRequires:  pipewire-devel
BuildRequires:  wireplumber-devel
# sd-bus: the laptop backlight through logind (docs/brightness.md §5.3), and
# vela-bluetooth (docs/bluetooth.md).
BuildRequires:  systemd-devel

Requires:       wlroots >= 0.20
# Tools required by the installed vela-update command even without a build.
Requires:       git
Requires:       util-linux
Requires:       rpm
Requires:       dnf5
Requires:       qt6-qtbase
Requires:       qt6-qtdeclarative
Requires:       qt6-qtsvg
Requires:       qt6-qtwayland
Requires:       layer-shell-qt
Requires:       pam
Requires:       polkit
Requires:       python3
Requires:       xorg-x11-server-Xwayland
Requires:       pipewire
Requires:       wireplumber
Requires:       hicolor-icon-theme

# Public, attested binary downloads (curl + cosign) and local RPM build fallback.
# Keep the build toolchain optional for systems using prebuilt releases.
Recommends:     curl
Recommends:     python3
# Cosign is an optional verification prerequisite; Fedora 44 does not ship it
# in the default repositories, so document the upstream RPM installation.
# No GitHub CLI is required by vela-update for public alpha releases.
Recommends:     rpm-build
Recommends:     dnf5-plugins
Recommends:     xdg-desktop-portal-wlr
Recommends:     xdg-desktop-portal-kde
Recommends:     breeze-icon-theme
Recommends:     NetworkManager
Recommends:     bluez
Recommends:     udisks2
Recommends:     kf6-kimageformats
Recommends:     qt6-qtmultimedia
Recommends:     power-profiles-daemon

%description
Vela is an experimental Wayland desktop environment with a custom compositor,
Vulkan renderer, Qt Quick shell, file manager, settings, and lock screen.

%prep
%autosetup -n vela-%{version}

%build
%cmake -G Ninja \
    %{?vela_ccache:-DCMAKE_C_COMPILER_LAUNCHER=ccache} \
    %{?vela_ccache:-DCMAKE_CXX_COMPILER_LAUNCHER=ccache} \
    -DCMAKE_INSTALL_LIBEXECDIR=libexec/vela \
    -DVELA_BUILD_TOOLS=OFF \
    -DVELA_PACKAGE_COMMIT=%{vela_commit}
%cmake_build

%install
%cmake_install
install -Dm755 packaging/vela-update %{buildroot}%{_bindir}/vela-update
install -Dm644 packaging/verify-attestation.py %{buildroot}%{_libexecdir}/vela/verify-attestation.py

%files
%license LICENSE
%doc README.md
%{_bindir}/vela-*
%{_libexecdir}/vela/
%{_datadir}/applications/vela-*.desktop
%{_datadir}/wayland-sessions/vela.desktop
%{_datadir}/xdg-desktop-portal/vela-portals.conf
%{_prefix}/lib/systemd/user/vela-session.target
%{_prefix}/lib/udev/rules.d/70-vela-i2c.rules
%{_prefix}/lib/modules-load.d/vela-i2c.conf
%config(noreplace) /etc/pam.d/vela-lock
%config(noreplace) /etc/xdg/xdg-desktop-portal-wlr/Vela

%changelog
* Wed Oct 14 2026 Vela contributors <dev@velade.org> - 0.1.0-1
- Initial experimental Fedora RPM packaging
