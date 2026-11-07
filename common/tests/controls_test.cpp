// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Vela.Controls' shared pieces under Qt Quick Test (docs/visual-coherence.md
// §6.8, §6.9): the tst_*.qml files next to this one. A private vela.conf
// sets the durations three times Vela's, so a fade can be caught halfway;
// with VELA_TEST_ANIMATION_EFFECTS=no it turns animation effects off, and the
// tests that need them on skip themselves (two ctest entries).

#include "controls.h"
#include "quicktest.h"

#include <QAccessible>
#include <QQmlEngine>
#include <QtQml/qqmlextensionplugin.h>

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
    Setup setup;
    return vela::test::quickTestMain(argc, argv, "controls", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "controls_test.moc"
