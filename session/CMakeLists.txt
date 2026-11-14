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
install(PROGRAMS vela-cpu-check DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
# A removable compatibility shim until Vela Secrets replaces external stores.
install(FILES credential-compat/select-backend credential-compat/ensure-backend
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/credential-compat)
# The build-tree compositor uses the same session hook as the installed one.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/compositor/credential-compat)
configure_file(credential-compat/ensure-backend
    ${CMAKE_BINARY_DIR}/compositor/credential-compat/ensure-backend COPYONLY)
install(FILES vela-gnome-keyring-compat.service vela-kwallet-compat.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
if(VELA_BUILD_TOOLS)
    add_test(NAME session.credential-compat
        COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/credential-compat.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/credential-compat/select-backend
            ${CMAKE_CURRENT_SOURCE_DIR}/credential-compat/ensure-backend)
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vela.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)
install(FILES vela-portals.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
install(FILES vela-session.target DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
# xdg-desktop-portal-wlr configuration when the desktop is Vela.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/xdpw-vela.conf DESTINATION /etc/xdg/xdg-desktop-portal-wlr RENAME Vela)
