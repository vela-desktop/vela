// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shortcutrunner.h"

#include "appmodel.h"
#include "processlaunch.h"

extern "C" {
#include "custom.h"
}

#include <QDir>
#include <QFile>

ShortcutRunner::ShortcutRunner(AppModel* apps, QObject* parent)
    : QObject(parent)
    , m_apps(apps)
{
}

ShortcutRunner::Target ShortcutRunner::find(const QString& path, const QString& combo)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    // Whole lines, all of them, as the compositor reads the file: a line cut
    // short could still match and run half a command.
    while (!file.atEnd()) {
        QByteArray line = file.readLine();
        if (line.endsWith('\n')) {
            line.chop(1);
        }
        vela_custom_line parsed {};
        char text[128];
        if (!vela_custom_line_parse(line.constData(), size_t(line.size()), &parsed)
            || !vela_combo_format(&parsed.combo, text, sizeof(text)) || combo != QLatin1String(text)) {
            continue;
        }
        return { QString::fromLatin1(vela_custom_kind_name(parsed.kind)),
            QString::fromUtf8(parsed.target, qsizetype(parsed.target_length)) };
    }
    return {};
}

bool ShortcutRunner::run(const QString& combo)
{
    char path[4096];
    const Target target = vela_custom_path(path, sizeof(path)) ? find(QString::fromUtf8(path), combo) : Target();
    if (target.kind.isEmpty()) {
        qWarning("vela-shell: no custom shortcut %s in shortcuts.conf", qPrintable(combo));
        return false;
    }
    qInfo(
        "vela-shell: custom shortcut %s: %s %s", qPrintable(combo), qPrintable(target.kind), qPrintable(target.target));
    if (target.kind == QLatin1String("app")) {
        return m_apps && m_apps->launchId(target.target);
    }
    if (target.kind == QLatin1String("command")) {
        return vela::process::startDetached(
            QStringLiteral("/bin/sh"), { QStringLiteral("-c"), target.target }, QDir::homePath());
    }
    return vela::process::startDetached(QStringLiteral("xdg-open"), { target.target }, QDir::homePath());
}
