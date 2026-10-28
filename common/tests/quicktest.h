// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What Vela's Qt Quick Test runners (the controls', the shell's, Settings')
// share: a private home, offscreen with Qt Quick's software renderer (no
// GPU in CI), and a vela.conf with durations three times Vela's, so a fade
// can be caught halfway; with VELA_TEST_ANIMATION_EFFECTS=no animation
// effects are off (two ctest entries per runner).

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>

namespace vela::test {

inline int quickTestMain(int argc, char** argv, const char* name, const char* sourceDir, QObject* setup)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
        qputenv("QT_QUICK_BACKEND", "software");
    }
    QDir().mkpath(home.filePath(QStringLiteral("config/vela")));
    QFile conf(home.filePath(QStringLiteral("config/vela/vela.conf")));
    if (!conf.open(QIODevice::WriteOnly)) {
        return 1;
    }
    conf.write("animation-duration=300\n");
    if (qEnvironmentVariable("VELA_TEST_ANIMATION_EFFECTS") == QLatin1String("no")) {
        conf.write("animation-effects=no\n");
    }
    conf.close();
    return quick_test_main_with_setup(argc, argv, name, sourceDir, setup);
}

} // namespace vela::test
