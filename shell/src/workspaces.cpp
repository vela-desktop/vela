// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "workspaces.h"

#include "shellcontroller.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>

Workspaces::Workspaces(QObject* parent)
    : QObject(parent)
    , m_names { QStringLiteral("Desktop 1") }
{
}

void Workspaces::query()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    QLocalSocket socket;
    socket.connectToServer(runtimeDir + QStringLiteral("/vela-") + display + QStringLiteral(".sock"));
    if (!socket.waitForConnected(200)) {
        return;
    }
    socket.write("workspaces\n");
    if (!socket.waitForBytesWritten(200)) {
        return;
    }
    QByteArray reply;
    while (!reply.contains('\n') && socket.waitForReadyRead(200)) {
        reply += socket.readAll();
    }
    update(reply.trimmed());
}

void Workspaces::update(const QByteArray& json)
{
    const QJsonObject state = QJsonDocument::fromJson(json).object();
    if (state.isEmpty()) {
        return;
    }
    m_current = state[QStringLiteral("current")].toInt();
    m_names.clear();
    for (const QJsonValue& name : state[QStringLiteral("names")].toArray()) {
        m_names << name.toString();
    }
    m_windows = state[QStringLiteral("windows")].toObject().toVariantMap();
    m_outputs = state[QStringLiteral("outputs")].toObject().toVariantMap();
    m_attention.clear();
    for (const QJsonValue& id : state[QStringLiteral("attention")].toArray()) {
        m_attention.insert(id.toString());
    }
    m_stickyApps.clear();
    for (const QJsonValue& app : state[QStringLiteral("stickyApps")].toArray()) {
        m_stickyApps << app.toString();
    }
    m_snapGroups = state[QStringLiteral("snapGroups")].toArray().toVariantList();
    emit changed();
}

int Workspaces::workspaceOf(const QString& window) const
{
    const auto it = m_windows.constFind(window);
    return it == m_windows.cend() ? -2 : it.value().toInt();
}

void Workspaces::switchTo(int index)
{
    ShellController::sendToCompositor("workspace switch " + QByteArray::number(index));
}

void Workspaces::create(bool switchTo)
{
    ShellController::sendToCompositor(switchTo ? "workspace new switch" : "workspace new");
}

void Workspaces::remove(int index)
{
    ShellController::sendToCompositor("workspace close " + QByteArray::number(index));
}

void Workspaces::rename(int index, const QString& name)
{
    ShellController::sendToCompositor("workspace rename " + QByteArray::number(index) + ' '
        + name.simplified().toUtf8());
}

void Workspaces::move(int from, int to)
{
    ShellController::sendToCompositor("workspace move " + QByteArray::number(from) + ' ' + QByteArray::number(to));
}

void Workspaces::moveWindow(const QString& window, int index)
{
    ShellController::sendToCompositor("window " + window.toLatin1() + " move-to " + QByteArray::number(index));
}

void Workspaces::moveWindowToNew(const QString& window)
{
    ShellController::sendToCompositor("window " + window.toLatin1() + " move-to-new");
}

void Workspaces::setWindowSticky(const QString& window, bool on)
{
    ShellController::sendToCompositor("window " + window.toLatin1() + (on ? " sticky" : " unsticky"));
}

void Workspaces::setAppSticky(const QString& window, bool on)
{
    ShellController::sendToCompositor("window " + window.toLatin1() + (on ? " app-sticky" : " app-unsticky"));
}
