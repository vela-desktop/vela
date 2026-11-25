// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The lock screen's look (lock/look.h): the effective accent, the
// expressive variant's roles and clock color, and the files read on a
// private configuration, a broken one included.

#include "../look.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

using vela::accent::Source;

void testContrast()
{
    check(std::abs(vela::lock::contrast(Qt::white, Qt::black) - 21.0) < 0.01, "white on black is 21:1");
    check(std::abs(vela::lock::contrast(QColor(90, 90, 90), QColor(90, 90, 90)) - 1.0) < 1e-9, "a color on itself is 1:1");
    check(vela::lock::contrast(Qt::white, Qt::black) == vela::lock::contrast(Qt::black, Qt::white), "contrast is symmetric");
}

void testAccent()
{
    const QColor blue(91, 140, 255);
    // Nothing set: Vela's blue, the default look, as before themes reached
    // the lock.
    vela::lock::Look look = vela::lock::lookFrom({}, false, Source::Theme, QColor(), QColor());
    check(look.accent == blue, "no theme and no choice: Vela's blue");
    check(!look.expressive, "no theme: the default look");
    check(vela::lock::Look().accent == blue, "the fallback look is Vela's blue");
    // The user's color, the wallpaper's on request, the theme's over both.
    look = vela::lock::lookFrom({}, false, Source::Theme, QColor(0, 200, 0), QColor(18, 52, 86));
    check(look.accent == QColor(0, 200, 0), "the chosen accent");
    look = vela::lock::lookFrom({}, false, Source::Wallpaper, QColor(0, 200, 0), QColor(18, 52, 86));
    check(look.accent == QColor(18, 52, 86), "the wallpaper's color when asked");
    look = vela::lock::lookFrom({}, false, Source::Wallpaper, QColor(0, 200, 0), QColor());
    check(look.accent == QColor(0, 200, 0), "no wallpaper color: the chosen one");
    const QVariantMap theme { { QStringLiteral("dark.accent"), QColor(200, 0, 0) } };
    look = vela::lock::lookFrom(theme, false, Source::Wallpaper, QColor(0, 200, 0), QColor(18, 52, 86));
    check(look.accent == QColor(200, 0, 0), "the theme's accent for the mode wins");
    look = vela::lock::lookFrom(theme, true, Source::Theme, QColor(0, 200, 0), QColor());
    check(look.accent == QColor(0, 200, 0), "a dark-only accent stays out of the light mode");
}

void testExpressiveRoles()
{
    const QVariantMap theme { { QStringLiteral("controls-variant"), QStringLiteral("expressive") } };
    const QColor chosen(0x67, 0x50, 0xa4);
    const vela::lock::Look look = vela::lock::lookFrom(theme, true, Source::Theme, chosen, QColor());
    check(look.expressive, "controls-variant=expressive");
    // The dark scheme in both modes: the lock is light on a dimmed wallpaper.
    const vela::color::Roles roles = vela::color::tonalSpot(chosen.rgb(), true);
    check(look.primary == vela::lock::fromArgb(roles.primary), "primary is the dark scheme's");
    check(look.primaryContainer == vela::lock::fromArgb(roles.primaryContainer), "primary container");
    check(look.surfaceContainerHighest == vela::lock::fromArgb(roles.surfaceContainerHighest), "the field's surface");
    check(vela::lock::contrast(look.onPrimaryContainer, look.primaryContainer) >= 4.5, "the avatar's initial reads");
    check(vela::lock::contrast(look.onSurface, look.surfaceContainerHighest) >= 4.5, "the password's dots read");
    check(vela::lock::contrast(look.onSurfaceVariant, look.surfaceContainerHighest) >= 4.5, "the field's hint reads");
    // A theme's primary starts the palette.
    QVariantMap withPrimary = theme;
    withPrimary.insert(QStringLiteral("primary"), QColor(0, 120, 60));
    const vela::lock::Look green = vela::lock::lookFrom(withPrimary, true, Source::Theme, chosen, QColor());
    check(green.primary == vela::lock::fromArgb(vela::color::tonalSpot(QColor(0, 120, 60).rgb(), true).primary),
        "the theme's primary is the palette's source");
}

void testClockColor()
{
    const QColor primary = vela::lock::lookFrom({ { QStringLiteral("controls-variant"), QStringLiteral("expressive") } },
        false, Source::Theme, QColor(91, 140, 255), QColor()).primary;
    // On a dark wallpaper the primary itself.
    check(vela::lock::clockColor(primary, QColor(20, 24, 40)) == primary, "on a dark wallpaper: primary");
    // On lighter ones a lighter primary that still reads at 3:1, or white.
    for (const QColor background : { QColor(110, 110, 110), QColor(150, 150, 150), QColor(170, 170, 60) }) {
        const QColor color = vela::lock::clockColor(primary, background);
        check(color == QColor(Qt::white) || vela::lock::contrast(color, background) >= 3.0,
            "the clock reads at 3:1 or is white");
        check(vela::lock::luminance(color) >= vela::lock::luminance(primary), "only ever lighter than primary");
    }
    check(vela::lock::clockColor(primary, QColor(230, 230, 230)) == QColor(Qt::white), "nothing reads: white");
    check(vela::lock::clockColor(QColor(), QColor(0, 0, 0)) == QColor(Qt::white), "no primary: white");
}

void testBrightestBehind()
{
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::black);
    for (int y = 0; y < 100; ++y) {
        for (int x = 150; x < 200; ++x) {
            image.setPixel(x, y, qRgb(255, 255, 255));
        }
    }
    // The clock's dimming: black at 60 alpha.
    check(vela::lock::brightestBehind(image, QRect(0, 0, 100, 100), 60) == QColor(0, 0, 0), "a black area");
    const QColor light = vela::lock::brightestBehind(image, QRect(100, 0, 100, 100), 60);
    check(light.red() == 195 && light.green() == 195 && light.blue() == 195, "the white part, dimmed");
    check(vela::lock::brightestBehind(image, QRect(300, 300, 10, 10), 60) == QColor(Qt::black), "outside: black");
    check(vela::lock::brightestBehind(QImage(), QRect(0, 0, 10, 10), 60) == QColor(Qt::black), "no image: black");
}

void write(const QString& path, const QByteArray& text)
{
    QDir().mkpath(path.section(u'/', 0, -2));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(text);
    }
}

void testConfiguredFiles()
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_DATA_DIRS", home.filePath(QStringLiteral("data")).toUtf8());
    const QString config = home.filePath(QStringLiteral("config"));

    // Nothing there: the default look.
    vela::lock::Look look = vela::lock::configuredLook();
    check(look.accent == QColor(91, 140, 255) && !look.expressive, "no files: the default look");

    write(config + QStringLiteral("/vela/themes/lock-test.conf"),
        "# Lock Test: expressive\nschema-version=1\ncontrols-variant=expressive\ncolor-source=wallpaper\n");
    write(config + QStringLiteral("/vela/vela.conf"), "theme=lock-test\n");
    write(config + QStringLiteral("/Vela/vela-shell.conf"), "[appearance]\naccent=#00c800\n");
    look = vela::lock::configuredLook();
    check(look.expressive, "vela.conf's theme is read");
    check(look.accent == QColor(0, 200, 0), "no wallpaper color yet: the chosen accent");

    write(home.filePath(QStringLiteral("cache/vela/wallpaper-color")), "#123456\n");
    look = vela::lock::configuredLook();
    check(look.accent == QColor(0x12, 0x34, 0x56), "the theme asks for the wallpaper's color");

    // Broken files: no theme, no accent, a cache of garbage.
    static const char garbage[] = "\x00\xff garbage\ncontrols-variant=\x01\n";
    write(config + QStringLiteral("/vela/themes/lock-test.conf"), QByteArray(garbage, sizeof(garbage) - 1));
    write(config + QStringLiteral("/Vela/vela-shell.conf"), "[appearance\naccent=not a color\n");
    write(home.filePath(QStringLiteral("cache/vela/wallpaper-color")), "#zz\n");
    look = vela::lock::configuredLook();
    check(!look.expressive, "a broken theme: the default look");
    check(look.accent.isValid(), "a broken configuration still has an accent");
}

} // namespace

int main()
{
    testContrast();
    testAccent();
    testExpressiveRoles();
    testClockColor();
    testBrightestBehind();
    testConfiguredFiles();
    if (failures == 0) {
        std::cout << "lock look: all tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
