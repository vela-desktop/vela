// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The lock screen's drawing (lock/main.cpp paints every output's buffer
// with it): the time and date over the wallpaper, or the user and the
// password field over the blurred wallpaper, in the theme's look (look.h).
// A pure function of its inputs, for lock/tests/test_lock_drawing.cpp.

#include "look.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QSizeF>

#include <algorithm>

namespace vela::lock {

enum class Mode { Clock, Login, Checking };

// What the lock shows besides the look.
struct Scene {
    Mode mode = Mode::Clock;
    QDateTime now;
    QString timeFormat;
    QString displayName;
    // The password typed so far: empty, or its characters (one dot each).
    bool passwordEmpty = true;
    qsizetype passwordChars = 0;
    QString message;
    // Caps Lock is on (the keyboard's locked modifiers): said under the
    // field while the password is asked (docs/keyboard.md §8.2).
    bool capsLock = false;
    // The Caps Lock display (§8.3): a pill at the bottom for a moment after
    // Caps Lock changes, in every mode. 0 is hidden; it fades to 0.
    double capsDisplay = 0;
    bool capsDisplayOn = false;
};

// The Caps Lock display's opacity `elapsed` ms after Caps Lock changed: whole
// for `hold` ms, then down to nothing in `fade` ms (at once if 0).
inline double capsDisplayOpacity(qint64 elapsed, qint64 hold, qint64 fade)
{
    if (elapsed < hold) {
        return 1;
    }
    if (fade <= 0 || elapsed >= hold + fade) {
        return 0;
    }
    return 1.0 - double(elapsed - hold) / double(fade);
}

// The Caps Lock key's arrow, in `box`: filled with a bar under it when
// Caps Lock is on, an outline when it is off (Breeze's
// keyboard-caps-locked and keyboard-caps-disabled, which the shell's
// display shows).
inline void paintCapsArrow(QPainter& p, const QRectF& box, const QColor& color, bool on)
{
    const double w = box.width();
    const double h = box.height();
    const double cx = box.center().x();
    const double arrowBottom = box.top() + h * (on ? 0.68 : 0.86);
    QPainterPath arrow;
    arrow.moveTo(cx, box.top() + h * 0.08);
    arrow.lineTo(box.left() + w * 0.92, box.top() + h * 0.48);
    arrow.lineTo(cx + w * 0.2, box.top() + h * 0.48);
    arrow.lineTo(cx + w * 0.2, arrowBottom);
    arrow.lineTo(cx - w * 0.2, arrowBottom);
    arrow.lineTo(cx - w * 0.2, box.top() + h * 0.48);
    arrow.lineTo(box.left() + w * 0.08, box.top() + h * 0.48);
    arrow.closeSubpath();
    p.save();
    if (on) {
        p.setPen(Qt::NoPen);
        p.fillPath(arrow, color);
        p.fillRect(QRectF(cx - w * 0.2, box.top() + h * 0.78, w * 0.4, h * 0.1), color);
    } else {
        p.setPen(QPen(color, std::max(1.0, w / 12), Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(arrow);
    }
    p.restore();
}

// The Caps Lock display at the bottom in the middle, where the shell shows
// it (CapsLockOsd.qml) with no taskbar under it: 52 tall, 48 from the
// bottom, lower when the login's lines reach there (`clearOf`, their
// bottom), as far as 8 from the edge. Expressive: a full pill on surface
// container highest, as the field; otherwise the field's translucent white
// over a dark pill.
inline void paintCapsDisplay(
    QPainter& p, QFont font, double lw, double lh, const Look& look, const Scene& scene, double clearOf = 0)
{
    if (scene.capsDisplay <= 0) {
        return;
    }
    font.setPixelSize(15);
    font.setWeight(QFont::Normal);
    const QString text = scene.capsDisplayOn ? QCoreApplication::translate("Lock", "Caps Lock on")
                                             : QCoreApplication::translate("Lock", "Caps Lock off");
    const double icon = 18;
    const double gap = 10;
    const double textWidth = QFontMetricsF(font).horizontalAdvance(text);
    const double width = std::max(180.0, icon + gap + textWidth + 40);
    const double top = std::min(std::max(lh - 48 - 52, clearOf + 8), lh - 8 - 52);
    const QRectF pill(lw / 2 - width / 2, top, width, 52);
    p.save();
    p.setOpacity(std::min(1.0, scene.capsDisplay));
    QPainterPath path;
    if (look.expressive) {
        path.addRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        p.fillPath(path, look.surfaceContainerHighest);
    } else {
        path.addRoundedRect(pill, 8, 8);
        p.fillPath(path, QColor(32, 32, 32, 230));
        p.setPen(QPen(QColor(255, 255, 255, 50), 1));
        p.setBrush(Qt::NoBrush); // the password's dots leave theirs
        p.drawPath(path);
    }
    const QColor ink = look.expressive ? look.onSurface : QColor(Qt::white);
    const double left = pill.center().x() - (icon + gap + textWidth) / 2;
    paintCapsArrow(p, QRectF(left, pill.center().y() - icon / 2, icon, icon), ink, scene.capsDisplayOn);
    p.setFont(font);
    p.setPen(ink);
    p.drawText(
        QRectF(left + icon + gap, pill.top(), textWidth + 1, pill.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    p.restore();
}

// The characters of a UTF-8 password (multibyte ones count as one).
inline qsizetype passwordCharacters(const std::string& password)
{
    return std::count_if(password.begin(), password.end(),
        [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
}

// Paint `image` (physical pixels: `logical` at scale120 / 120) with the
// wallpaper already scaled and darkened (`background`) and its blur, both
// of the image's size.
inline void paint(QImage& image, const QImage& background, const QImage& blurred, QSizeF logical, uint32_t scale120,
    const Look& look, const Scene& scene)
{
    const int w = image.width();
    const int h = image.height();
    QPainter p(&image);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    const bool login = scene.mode != Mode::Clock;
    p.drawImage(0, 0, login ? blurred : background);
    p.fillRect(QRect(0, 0, w, h), QColor(0, 0, 0, login ? 110 : 60));

    // From here on in logical coordinates.
    const double scale = double(scale120) / 120.0;
    p.scale(scale, scale);
    const double lw = logical.width();
    const double lh = logical.height();
    QFont font = QGuiApplication::font();
    double loginBottom = 0; // where the login's last line ends

    if (!login) {
        const QDateTime& now = scene.now;
        const QString time = QLocale().toString(now.time(), scene.timeFormat);
        // Expressive: the second hero moment, a larger time in Bold, in
        // the accent's primary. 160 and its 200-tall box are Vela's
        // choices (M3's display scale stops at 57; this is a poster).
        const int timeSize = look.expressive ? 160 : 120;
        font.setPixelSize(timeSize);
        font.setWeight(look.expressive ? QFont::Bold : QFont::DemiBold);
        // On a narrow output (a portrait phone-sized one) the time,
        // longer at 12 hours ("12:45 PM"), shrinks to 90% of its width.
        const qreal wide = QFontMetricsF(font).horizontalAdvance(time);
        if (wide > lw * 0.9) {
            font.setPixelSize(std::max(24, int(timeSize * lw * 0.9 / wide)));
        }
        p.setFont(font);
        const QRectF timeBox(0, lh * 0.12, lw, look.expressive ? 200 : 150);
        QColor timeColor(255, 255, 255, 240);
        if (look.expressive) {
            // Primary, lighter where the wallpaper behind the time is
            // light (3:1 against its dimmed colors: the sharp wallpaper's,
            // which the clock is drawn over), else white.
            const QFontMetricsF metrics(font);
            const qreal textWidth = metrics.horizontalAdvance(time);
            const QRectF text((lw - textWidth) / 2, timeBox.top(), textWidth, metrics.height());
            const QRect physical(int(text.left() * scale), int(text.top() * scale), int(text.width() * scale),
                int(text.height() * scale));
            timeColor = clockColor(look.primary,
                brightestBehind(background, physical, 60));
        }
        p.setPen(timeColor);
        p.drawText(timeBox, Qt::AlignHCenter | Qt::AlignTop, time);
        p.setPen(QColor(255, 255, 255, 240));
        font.setPixelSize(26);
        font.setWeight(QFont::Medium);
        p.setFont(font);
        const QString date = QLocale().toString(now.date(), QStringLiteral("dddd d MMMM"));
        // The date too, on a narrow output: a long day and month in a wide
        // font ran past both edges.
        const qreal dateWide = QFontMetricsF(font).horizontalAdvance(date);
        if (dateWide > lw * 0.9) {
            font.setPixelSize(std::max(12, int(26 * lw * 0.9 / dateWide)));
            p.setFont(font);
        }
        p.drawText(QRectF(0, timeBox.bottom() + 4, lw, 40), Qt::AlignHCenter | Qt::AlignTop, date);
    } else {
        // User in the center, password below.
        const double cx = lw / 2;
        const double top = lh * 0.30;
        const double avatar = 150;
        QPainterPath circle;
        circle.addEllipse(QPointF(cx, top + avatar / 2), avatar / 2, avatar / 2);
        // Expressive: M3's avatar, primary container and its text.
        p.fillPath(circle, look.expressive ? look.primaryContainer : look.accent);
        font.setPixelSize(64);
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(look.expressive ? look.onPrimaryContainer : QColor(Qt::white));
        p.drawText(QRectF(cx - avatar / 2, top, avatar, avatar), Qt::AlignCenter, scene.displayName.left(1).toUpper());

        font.setPixelSize(28);
        p.setFont(font);
        p.setPen(Qt::white);
        const double nameY = top + avatar + 20;
        p.drawText(QRectF(0, nameY, lw, 40), Qt::AlignHCenter | Qt::AlignTop, scene.displayName);

        // Expressive: a full pill on surface container highest, 48 tall
        // (Vela's choice: M3's text field is 56, the desktop's denser),
        // without outline or line.
        const QRectF field(cx - 150, nameY + 60, 300, look.expressive ? 48 : 40);
        QPainterPath fieldPath;
        if (look.expressive) {
            fieldPath.addRoundedRect(field, field.height() / 2, field.height() / 2);
            p.fillPath(fieldPath, look.surfaceContainerHighest);
        } else {
            fieldPath.addRoundedRect(field, 6, 6);
            p.fillPath(fieldPath, QColor(255, 255, 255, 36));
            p.setPen(QPen(QColor(255, 255, 255, 50), 1));
            p.drawPath(fieldPath);
            // The accent line at the bottom, as in Windows 11 fields.
            p.fillRect(QRectF(field.left() + 2, field.bottom() - 2, field.width() - 4, 2), look.accent);
        }

        font.setPixelSize(15);
        font.setWeight(QFont::Normal);
        p.setFont(font);
        // Expressive: the text 24 from the pill's ends (M3's text
        // field's 16 plus the corner's round, Vela's choice).
        const QRectF textBox = look.expressive ? field.adjusted(24, 0, -24, 0) : field.adjusted(14, 0, -14, 0);
        if (scene.mode == Mode::Checking) {
            p.setPen(look.expressive ? look.onSurfaceVariant : QColor(255, 255, 255, 170));
            p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, QCoreApplication::translate("Lock", "Signing in…"));
        } else if (scene.passwordEmpty) {
            p.setPen(look.expressive ? look.onSurfaceVariant : QColor(255, 255, 255, 140));
            p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, QCoreApplication::translate("Lock", "Password"));
        } else {
            const QColor dots = look.expressive ? look.onSurface : QColor(Qt::white);
            p.setPen(dots);
            p.setBrush(dots);
            const double dot = 7;
            for (qsizetype i = 0; i < std::min<qsizetype>(scene.passwordChars, 32); ++i) {
                p.drawEllipse(QPointF(textBox.left() + dot / 2 + i * (dot + 5), textBox.center().y()), dot / 2, dot / 2);
            }
        }
        loginBottom = field.bottom();
        if (!scene.message.isEmpty()) {
            p.setPen(QColor(255, 255, 255, 220));
            p.drawText(QRectF(0, field.bottom() + 16, lw, 30), Qt::AlignHCenter | Qt::AlignTop, scene.message);
            loginBottom = field.bottom() + 16 + QFontMetricsF(font).height();
        }
        // Below the message, if there is one, a line of the font further
        // down, in the caution color of the dark theme: the lock is always
        // drawn over a darkened wallpaper.
        if (scene.capsLock && scene.mode == Mode::Login) {
            const double line = QFontMetricsF(font).lineSpacing() + 6;
            const double y = field.bottom() + 16 + (scene.message.isEmpty() ? 0 : line);
            p.setPen(QColor(0xfc, 0xe1, 0x00));
            p.drawText(QRectF(0, y, lw, 30), Qt::AlignHCenter | Qt::AlignTop,
                QCoreApplication::translate("Lock", "Caps Lock is on."));
            loginBottom = y + QFontMetricsF(font).height();
        }
    }
    paintCapsDisplay(p, QGuiApplication::font(), lw, lh, look, scene, loginBottom);
}

} // namespace vela::lock
