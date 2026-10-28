// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' own QML pieces under Qt Quick Test (the tst_*.qml files next to
// this one), as Vela.Controls' runner does, plus a fake Vela.Bluetooth
// (common/bluetooth/tests/FakeBluetooth.qml) for the Bluetooth page's
// pieces. A private vela.conf sets the durations three times Vela's; with
// VELA_TEST_ANIMATION_EFFECTS=no it turns animation effects off (two ctest
// entries).

#include "controls.h"
#include "quicktest.h"

#include <QAccessible>
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
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(FAKE_BLUETOOTH)), "Vela.Bluetooth", 1, 0,
            "Bluetooth");
    }
};

int main(int argc, char** argv)
{
    Setup setup;
    return vela::test::quickTestMain(argc, argv, "settings", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "settings_test.moc"
