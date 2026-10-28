// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's own QML pieces under Qt Quick Test (the tst_*.qml files next
// to this one), as Vela.Controls' runner does, plus what the shell's
// windows find in their context: a fake Vela.Bluetooth
// (common/bluetooth/tests/FakeBluetooth.qml), `Shell` (FakeShell.qml) and
// `BluetoothPrompts` (FakeBluetoothPrompts.qml). A private vela.conf sets
// the durations three times Vela's; with VELA_TEST_ANIMATION_EFFECTS=no it
// turns animation effects off (two ctest entries).

#include "controls.h"

#include <QAccessible>
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QtQml/qqmlextensionplugin.h>
#include <QtQuickTest/quicktest.h>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)
Q_IMPORT_QML_PLUGIN(Vela_Bluetooth_UiPlugin)

class Setup : public QObject {
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        // Controls set their accessible role and state only once a screen
        // reader is there.
        QAccessible::setActive(true);
        vela::controls::install(*engine, Appearance::Mode::Apps);
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(FAKE_BLUETOOTH)), "Vela.Bluetooth", 1, 0, "Bluetooth");
        for (const auto& [name, file] : { std::pair { "Shell", "FakeShell.qml" }, std::pair { "BluetoothPrompts", "FakeBluetoothPrompts.qml" } }) {
            QQmlComponent component(engine, QUrl::fromLocalFile(QStringLiteral(QUICK_TEST_SOURCE_DIR "/") + QLatin1String(file)));
            QObject* fake = component.create();
            if (!fake) {
                qFatal("%s: %s", file, qPrintable(component.errorString()));
            }
            fake->setParent(engine);
            engine->rootContext()->setContextProperty(QLatin1String(name), fake);
        }
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
    return quick_test_main_with_setup(argc, argv, "shell", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "shell_test.moc"
