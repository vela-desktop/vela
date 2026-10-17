// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Vela.Controls' shared pieces under Qt Quick Test (docs/visual-coherence.md
// §6.8, §6.9): the tst_*.qml files next to this one. A private vela.conf
// sets the durations three times Vela's, so a fade can be caught halfway;
// with VELA_TEST_ANIMATION_EFFECTS=no it turns animation effects off, and the
// tests that need them on skip themselves (two ctest entries).

#include "controls.h"

#include <QAccessible>
#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QtQml/qqmlextensionplugin.h>
#include <QtQuickTest/quicktest.h>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

class Setup : public QObject {
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        // Controls set their accessible role and state only once a screen
        // reader is there.
        QAccessible::setActive(true);
        vela::controls::install(*engine, Appearance::Mode::Apps);
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // No GPU in CI: Qt Quick's software renderer draws the same items.
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

    Setup setup;
    return quick_test_main_with_setup(argc, argv, "controls", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "controls_test.moc"
