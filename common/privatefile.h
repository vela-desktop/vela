// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileDevice>
#include <QSaveFile>

// QSaveFile can preserve the permissions of an existing destination.
// Enforce owner-only access before writing sensitive settings or screenshots.
namespace vela {
inline bool openPrivateFile(QSaveFile& file, QIODevice::OpenMode mode = QIODevice::WriteOnly)
{
    if (!file.open(mode)) {
        return false;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }
    return true;
}
} // namespace vela
