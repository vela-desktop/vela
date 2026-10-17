// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QStringList>
#include <QVariantMap>

namespace vela::files {

// The shell forwards file operations to vela-files. A dedicated Explorer
// window owns the FileOps worker, conflict prompts, progress and cancellation.
// Neither clipboard paste nor desktop drops may launch cp/mv behind its back.
enum class TransferAction : int { Automatic = 0, Copy = 1, Move = 2 };

struct TransferRequest {
    TransferAction action = TransferAction::Automatic;
    QString directory;
    QStringList sources;
};

// Positional paths follow "--" so filenames beginning with '-' remain safe.
// Keep encoding and decoding here, shared by the shell and Explorer.
inline QStringList transferArguments(TransferAction action, const QString& directory, const QStringList& sources)
{
    const QString mode = action == TransferAction::Copy ? QStringLiteral("copy")
        : action == TransferAction::Move ? QStringLiteral("move") : QStringLiteral("auto");
    QStringList arguments { QStringLiteral("--vela-transfer"), mode, directory, QStringLiteral("--") };
    arguments.append(sources);
    return arguments;
}

inline bool parseTransferArguments(const QStringList& args, TransferRequest* out)
{
    if (!out || args.size() < 5 || args.at(0) != QLatin1String("--vela-transfer")
        || args.at(3) != QLatin1String("--") || args.at(2).isEmpty()) {
        return false;
    }
    TransferAction action;
    if (args.at(1) == QLatin1String("copy")) {
        action = TransferAction::Copy;
    } else if (args.at(1) == QLatin1String("move")) {
        action = TransferAction::Move;
    } else if (args.at(1) == QLatin1String("auto")) {
        action = TransferAction::Automatic;
    } else {
        return false;
    }
    if (args.mid(4).contains(QString())) {
        return false;
    }
    *out = { action, args.at(2), args.mid(4) };
    return true;
}

// Both the shell (spawn failed) and Explorer (invalid startup request) use a
// visible desktop notification, rather than relying on journal-only errors.
inline void notifyTransferStartupFailure(const QString& detail)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("Notify"));
    message << QStringLiteral("Vela Files") << uint(0) << QStringLiteral("dialog-error")
            << QCoreApplication::translate("Files", "File transfer did not start")
            << detail << QStringList {} << QVariantMap {} << 10000;
    QDBusConnection::sessionBus().asyncCall(message);
}

} // namespace vela::files
