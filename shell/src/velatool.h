// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

namespace vela {

// The running build's apps must win over an older installed package. Installed
// sessions find their companions beside the executable; PATH is only a fallback.
inline QString toolExecutable(const QString& name, const QString& siblingDir,
    const QString& appDir, const QString& appName)
{
    // Files and Settings both use SystemActions. Never launch ourselves.
    if (name == appName)
        return {};

    for (const QString& candidate : {
             appDir + QStringLiteral("/../") + siblingDir + u'/' + name,
             appDir + u'/' + name,
         }) {
        const QFileInfo file(candidate);
        if (file.isFile() && file.isExecutable())
            return file.canonicalFilePath();
    }

    return QStandardPaths::findExecutable(name);
}

// Desktop-file launches (Start menu, pinned apps, shortcuts) may intentionally
// start a second instance of Files or Settings, unlike SystemActions. Rewrite
// only bare Vela executable names; leave other apps and explicit paths alone.
inline QString desktopExecutable(const QString& program, const QString& appDir)
{
    QString siblingDir;
    if (program == QLatin1String("vela-files"))
        siblingDir = QStringLiteral("explorer");
    else if (program == QLatin1String("vela-settings"))
        siblingDir = QStringLiteral("settings");
    else
        return program;

    const QString resolved = toolExecutable(program, siblingDir, appDir, {});
    return resolved.isEmpty() ? program : resolved;
}

} // namespace vela
