// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "peek.h"

#include <QMimeDatabase>

namespace peek {

QString classify(const QString& mime, bool isDir, bool imageReadable, bool pdfSupported, bool mediaSupported)
{
    static const QMimeDatabase mimes;
    if (isDir) {
        return QStringLiteral("folder");
    }
    // Before images: QtPdf's image plugin makes a PDF readable as its first page.
    if (pdfSupported && mime == QLatin1String("application/pdf")) {
        return QStringLiteral("pdf");
    }
    if (imageReadable) {
        return QStringLiteral("image");
    }
    if (mediaSupported && (mime.startsWith(QLatin1String("video/")) || mime.startsWith(QLatin1String("audio/")))) {
        return QStringLiteral("media");
    }
    const QMimeType type = mimes.mimeTypeForName(mime);
    if ((type.isValid() && type.inherits(QStringLiteral("text/plain"))) || mime == QLatin1String("application/json")
        || mime == QLatin1String("application/xml") || mime == QLatin1String("application/x-shellscript")) {
        return QStringLiteral("text");
    }
    return QStringLiteral("other");
}

bool looksLikeText(const QByteArray& head)
{
    if (head.isEmpty() || head.contains('\0')) {
        return false;
    }
    // Invalid UTF-8 decodes to U+FFFD; the read may have cut the last
    // sequence (at most 3 bytes of it), and the file may hold U+FFFD itself.
    const bool hasReplacement = head.contains("\xEF\xBF\xBD");
    for (int drop = 0; drop <= 3 && drop < head.size(); ++drop) {
        if (hasReplacement || !QString::fromUtf8(head.chopped(drop)).contains(QChar::ReplacementCharacter)) {
            return true;
        }
    }
    return false;
}

QString truncateText(const QByteArray& head, qint64 fileSize, int maxBytes, bool* truncated)
{
    QByteArray bytes = head.left(maxBytes);
    const bool cut = fileSize > bytes.size();
    if (truncated) {
        *truncated = cut;
    }
    if (cut) {
        // Drop an incomplete UTF-8 sequence at the end (up to 3 bytes).
        int i = bytes.size() - 1, back = 0;
        while (i >= 0 && back < 3 && (uchar(bytes[i]) & 0xC0) == 0x80) {
            --i;
            ++back;
        }
        if (i >= 0 && uchar(bytes[i]) >= 0xC0) {
            const uchar lead = uchar(bytes[i]);
            const int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (back + 1 < need) {
                bytes.truncate(i);
            }
        }
    }
    QString text = QString::fromUtf8(bytes);
    text.replace(u'\t', QStringLiteral("    "));
    return text;
}

} // namespace peek
