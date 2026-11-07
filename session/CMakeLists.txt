# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# The session: SDDM entry, startup, ties to systemd and the portals.

configure_file(vela-session.in vela-session @ONLY)
configure_file(vela.desktop.in vela.desktop @ONLY)
configure_file(xdpw-vela.conf.in xdpw-vela.conf @ONLY)
# Also next to the compositor in the build directory, where it looks first.
configure_file(vela-session-env ${CMAKE_BINARY_DIR}/compositor/vela-session-env COPYONLY)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/vela-session DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS vela-session-env DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vela.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)
install(FILES vela-portals.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
install(FILES vela-session.target vela-gnome-keyring.service vela-kwallet.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
# xdg-desktop-portal-wlr configuration when the desktop is Vela.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/xdpw-vela.conf DESTINATION /etc/xdg/xdg-desktop-portal-wlr RENAME Vela)
