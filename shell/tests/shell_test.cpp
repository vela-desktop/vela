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
#include "quicktest.h"

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQml/qqmlextensionplugin.h>

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
    Setup setup;
    return vela::test::quickTestMain(argc, argv, "shell", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "shell_test.moc"
