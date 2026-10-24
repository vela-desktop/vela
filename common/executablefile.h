// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

// Recognize runnable ELF files (including AppImages), not ordinary documents
// or libraries. The execute bit is intentionally NOT checked here: callers
// must be able to report a useful error for ELF programs missing chmod +x.
namespace vela::files {

inline bool isProgram(const QFileInfo& info)
{
    if (!info.isFile()) {
        return false;
    }

    // Shared objects use .so or a version suffix such as .so.6.1. A simple
    // contains(".so") would reject legitimate names like my.software.AppImage.
    static const QRegularExpression librarySuffix(QStringLiteral(R"(\.so(?:\.[0-9]+)*$)"));
    if (librarySuffix.match(info.fileName()).hasMatch()) {
        return false;
    }

    QFile file(info.absoluteFilePath());
    return file.open(QIODevice::ReadOnly) && file.read(4) == QByteArray("\x7f" "ELF", 4);
}

} // namespace vela::files
