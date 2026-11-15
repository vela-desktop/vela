// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accessibleprobe.h"
#include "controls.h"
#include "quicktest.h"

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQml/qqmlextensionplugin.h>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

class Setup : public QObject {
    Q_OBJECT
public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        QAccessible::setActive(true);
        vela::test::AccessibleProbe::install();
        vela::controls::install(*engine, Appearance::Mode::Apps);
        QQmlComponent component(engine, QUrl::fromLocalFile(QStringLiteral(QUICK_TEST_SOURCE_DIR "/FakeContext.qml")));
        QObject* context = component.create();
        if (!context) {
            qFatal("%s", qPrintable(component.errorString()));
        }
        context->setParent(engine);
        for (const char* name : { "Config", "Tasks", "Tray", "Shell", "Transfers", "Sharing",
                 "Audio", "Access", "Status", "Notifications", "Effects", "Apps", "Jumps", "Sound" }) {
            const QByteArray property = QByteArray(name).toLower();
            engine->rootContext()->setContextProperty(QLatin1String(name), context->property(property).value<QObject*>());
        }
    }
};

int main(int argc, char** argv)
{
    Setup setup;
    return vela::test::quickTestMain(argc, argv, "taskbar", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "taskbar_test.moc"
