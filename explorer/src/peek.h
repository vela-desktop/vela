// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

// Quick preview (Space): pure logic, kept apart so the tests need no UI.
namespace peek {

constexpr int maxTextBytes = 64 * 1024;

// "folder", "image", "text", "pdf" or "other".
QString classify(const QString& mime, bool isDir, bool imageReadable, bool pdfSupported, bool mediaSupported);

// A file whose type says nothing (no extension, an unknown one) is text when
// its beginning has no NUL byte and is valid UTF-8.
constexpr int sniffBytes = 4096;
bool looksLikeText(const QByteArray& head);

// The beginning of a text file: at most `maxBytes`, never cutting a UTF-8
// sequence in half, tabs as spaces. `truncated` says whether the file went on.
QString truncateText(const QByteArray& head, qint64 fileSize, int maxBytes, bool* truncated = nullptr);

} // namespace peek
