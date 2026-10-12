// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// How the shell and Settings name the apps that play or record
// (docs/audio.md §5.2): a stream's ids matched to a .desktop file with the
// same rules in both processes (AppModel::findDesktopId).

#include "appmodel.h"
#include "audio.h"

inline vela::audio::Audio::AppResolver audioAppResolver(const AppModel& apps)
{
    return [&apps](const vela::audio::AppIdentity& id) {
        for (const QString& candidate : { id.portalAppId, id.id, id.binary, id.name }) {
            const QString desktopId = apps.findDesktopId(candidate);
            if (!desktopId.isEmpty()) {
                const QVariantMap entry = apps.entry(desktopId);
                return vela::audio::AppInfo { desktopId, entry.value(QStringLiteral("name")).toString(),
                    entry.value(QStringLiteral("iconName")).toString() };
            }
        }
        return vela::audio::AppInfo {};
    };
}
