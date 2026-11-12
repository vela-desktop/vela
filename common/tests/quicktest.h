// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What Vela's Qt Quick Test runners (the controls', the shell's, Settings',
// Files') share: a private home, offscreen with Qt Quick's software renderer
// (no GPU in CI), and a vela.conf with durations three times Vela's, so a
// fade can be caught halfway; with VELA_TEST_ANIMATION_EFFECTS=no animation
// effects are off (two ctest entries per runner). With
// VELA_TEST_THEME=expressive the private config also has a theme with the
// expressive controls and motion (controls-variant and motion-variant
// expressive) and the shell's expressive variants (quicksettings-,
// startmenu- and notifications-variant), and vela.conf chooses it: the
// *-expressive ctest entries, which run the tests in the suite's
// expressive/ folder (-input). VELA_TEST_THEME=controls, motion, color or
// shell sets one piece alone (controls-variant, motion-variant,
// color-source=wallpaper with a wallpaper color of #2e7d32 in the cache, or
// the three shell variants): the *-expressive-<piece> entries, on the same
// folder, whose tests skip what their theme doesn't set.

#include "renderingpolicy.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>

namespace vela::test {

/** Write the private vela.conf (and the expressive theme, when asked) under `home`'s config/. */
inline bool writeConfig(const QTemporaryDir& home)
{
    const QString vela = home.filePath(QStringLiteral("config/vela"));
    QDir().mkpath(vela);
    QFile conf(vela + QStringLiteral("/vela.conf"));
    if (!conf.open(QIODevice::WriteOnly)) {
        return false;
    }
    conf.write("animation-duration=300\n");
    if (qEnvironmentVariable("VELA_TEST_ANIMATION_EFFECTS") == QLatin1String("no")) {
        conf.write("animation-effects=no\n");
    }
    // The expressive test themes: the whole look, or one of its keys alone
    // (the mixed combinations: each piece inside an otherwise default theme).
    const QString which = qEnvironmentVariable("VELA_TEST_THEME");
    const QByteArray keys = which == QLatin1String("expressive")
        ? "controls-variant=expressive\n"
          "motion-variant=expressive\n"
          "quicksettings-variant=expressive\n"
          "startmenu-variant=expressive\n"
          "notifications-variant=expressive\n"
        : which == QLatin1String("controls") ? "controls-variant=expressive\n"
        : which == QLatin1String("motion") ? "motion-variant=expressive\n"
        : which == QLatin1String("color") ? "color-source=wallpaper\n"
        : which == QLatin1String("shell") ? "quicksettings-variant=expressive\n"
                                            "startmenu-variant=expressive\n"
                                            "notifications-variant=expressive\n"
                                          : QByteArray();
    if (!keys.isEmpty()) {
        QDir().mkpath(vela + QStringLiteral("/themes"));
        QFile theme(vela + QStringLiteral("/themes/vela-test-expressive.conf"));
        if (!theme.open(QIODevice::WriteOnly)) {
            return false;
        }
        theme.write("# Vela Test Expressive: some of the expressive keys, nothing else\n"
                    "schema-version=1\n"
                    + keys);
        conf.write("theme=vela-test-expressive\n");
    }
    if (which == QLatin1String("color")) {
        // The wallpaper's color, as the shell leaves it (mica.h).
        const QString cache = home.filePath(QStringLiteral("cache/vela"));
        QDir().mkpath(cache);
        QFile color(cache + QStringLiteral("/wallpaper-color"));
        if (!color.open(QIODevice::WriteOnly)) {
            return false;
        }
        color.write("#2e7d32\n");
    }
    return true;
}

/** Run QML tests with private configuration, controlled motion and the production text policy. */
inline int quickTestMain(int argc, char** argv, const char* name, const char* sourceDir, QObject* setup)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
        qputenv("QT_QUICK_BACKEND", "software");
    }
    if (!writeConfig(home)) {
        return 1;
    }
    vela::rendering::configureApplication();
    return quick_test_main_with_setup(argc, argv, name, sourceDir, setup);
}

} // namespace vela::test
