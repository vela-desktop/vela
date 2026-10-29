// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QDateTime>
#include <QRect>
#include <QString>
#include <QStringList>

// The small logic of the Snipping Tool's delay timer, color picker and screen
// recording (the wf-recorder command line).
namespace SnipExtras {

// The next delay in the cycle None -> 3 -> 5 -> 10 -> None (seconds).
inline int nextDelay(int seconds)
{
    switch (seconds) {
    case 0: return 3;
    case 3: return 5;
    case 5: return 10;
    default: return 0;
    }
}

// "#RRGGBB", upper case, ignoring alpha.
inline QString hexColor(const QColor& color)
{
    return color.name(QColor::HexRgb).toUpper();
}

// The recording area for wf-recorder -g: "x,y WxH" in layout coordinates.
inline QString recordGeometry(const QRect& area)
{
    return QStringLiteral("%1,%2 %3x%4").arg(area.x()).arg(area.y()).arg(area.width()).arg(area.height());
}

// "Screen Recording 2026-10-14 15-04-05.mp4" (no ':' in the name).
inline QString recordFileName(const QDateTime& when, bool webm)
{
    return QStringLiteral("Screen Recording ") + when.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss"))
        + (webm ? QStringLiteral(".webm") : QStringLiteral(".mp4"));
}

// The next sound source in the cycle off -> microphone (system sound: later).
inline bool nextAudio(bool microphone)
{
    return !microphone;
}

// The wf-recorder arguments. `output` set: that whole output (-o); else the
// rectangle `area` (global layout coordinates). mp4 is H.264; webm is the
// fallback when there is no libx264 (VP8 + Opus, as mp4's AAC doesn't fit).
inline QStringList recordArgs(const QString& file, const QString& output, const QRect& area, bool microphone, bool webm)
{
    QStringList args { QStringLiteral("-f"), file };
    if (!output.isEmpty()) {
        args << QStringLiteral("-o") << output;
    } else {
        args << QStringLiteral("-g") << recordGeometry(area);
    }
    if (microphone) {
        args << QStringLiteral("--audio");
    }
    if (webm) {
        args << QStringLiteral("-c") << QStringLiteral("libvpx");
        if (microphone) {
            args << QStringLiteral("-C") << QStringLiteral("libopus");
        }
    } else {
        args << QStringLiteral("-c") << QStringLiteral("libx264");
    }
    return args;
}

} // namespace SnipExtras
