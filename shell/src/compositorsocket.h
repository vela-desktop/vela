// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The compositor's command socket, one command per line: see
// vela_commands_listen() (compositor/src/command.c). Header-only, so that a
// test can link a model that talks to the compositor without the whole
// ShellController.

#include <QByteArray>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QString>

namespace compositor {

inline QString socketPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/vela-")
        + qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0")) + QStringLiteral(".sock");
}

// Connected and `command` written, or false within `timeoutMs` per step.
inline bool send(QLocalSocket& socket, const QByteArray& command, int timeoutMs)
{
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(timeoutMs)) {
        return false;
    }
    const QByteArray line = command + '\n';
    return socket.write(line) == line.size() && (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(timeoutMs));
}

// A command and its one-line reply, trimmed: empty when the compositor
// doesn't answer, each step within `timeoutMs`.
inline QByteArray ask(const QByteArray& command, int timeoutMs)
{
    QLocalSocket socket;
    if (!send(socket, command, timeoutMs)) {
        return {};
    }
    QByteArray reply;
    while (!reply.contains('\n') && socket.waitForReadyRead(timeoutMs)) {
        reply += socket.readAll();
    }
    return reply.trimmed();
}

} // namespace compositor
