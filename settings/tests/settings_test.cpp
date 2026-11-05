// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' own QML pieces under Qt Quick Test (the tst_*.qml files next to
// this one), as Vela.Controls' runner does, plus a fake Vela.Bluetooth
// (common/bluetooth/tests/FakeBluetooth.qml) for the Bluetooth page's
// pieces and a fake Vela.Updates (common/updates/tests/FakeUpdates.qml) for
// the Updates page's, and a fake Prefs (tests/FakePrefs.qml) for
// Personalization's. A private vela.conf sets the durations three times Vela's; with
// VELA_TEST_ANIMATION_EFFECTS=no it turns animation effects off (two ctest
// entries).

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
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(FAKE_BLUETOOTH)), "Vela.Bluetooth", 1, 0,
            "Bluetooth");
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(FAKE_UPDATES)), "Vela.Updates", 1, 0, "Updates");
        // Settings' Prefs is a context property (main.cpp): so is its fake.
        QQmlComponent component(engine, QUrl::fromLocalFile(QStringLiteral(FAKE_PREFS)));
        QObject* prefs = component.create();
        if (!prefs) {
            qFatal("FakePrefs.qml: %s", qPrintable(component.errorString()));
        }
        prefs->setParent(engine);
        engine->rootContext()->setContextProperty(QStringLiteral("Prefs"), prefs);
    }
};

int main(int argc, char** argv)
{
    Setup setup;
    return vela::test::quickTestMain(argc, argv, "settings", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "settings_test.moc"
