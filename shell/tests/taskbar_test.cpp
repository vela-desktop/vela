// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accessibleprobe.h"
#include "config.h"
#include "controls.h"
#include "quicktest.h"

#include <QAccessible>
#include <QLocale>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSaveFile>
#include <QSettings>
#include <QtQml/qqmlextensionplugin.h>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

class Setup : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE bool writeShellConfig(const QString& text)
    {
        QSaveFile file(QSettings().fileName());
        const QByteArray bytes = text.toUtf8();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    }

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        QAccessible::setActive(true);
        vela::test::AccessibleProbe::install();
        vela::controls::install(*engine, Appearance::Mode::Apps);
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(SHELL_STYLE_SOURCE)),
            "Vela.Shell.Test", 1, 0, "Styles");
        QQmlComponent component(engine, QUrl::fromLocalFile(QStringLiteral(QUICK_TEST_SOURCE_DIR "/FakeContext.qml")));
        QObject* context = component.create();
        if (!context) {
            qFatal("%s", qPrintable(component.errorString()));
        }
        context->setParent(engine);
        for (const char* name : { "Config", "Tasks", "Tray", "Shell", "Transfers", "Sharing",
                 "Audio", "Access", "Status", "Notifications", "Effects", "Apps", "Jumps", "Sound", "Capture" }) {
            const QByteArray property = QByteArray(name).toLower();
            engine->rootContext()->setContextProperty(QLatin1String(name), context->property(property).value<QObject*>());
        }
        if (qEnvironmentVariable("VELA_TEST_SHELL_CONFIG") == QLatin1String("real")) {
            QCoreApplication::setOrganizationName(QStringLiteral("Vela"));
            QCoreApplication::setApplicationName(QStringLiteral("vela-shell"));
            QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
            engine->rootContext()->setContextProperty(QStringLiteral("Config"), new Config(engine));
            engine->rootContext()->setContextProperty(QStringLiteral("SettingsFile"), this);
        }
    }
};

int main(int argc, char** argv)
{
    Setup setup;
    return vela::test::quickTestMain(argc, argv, "taskbar", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "taskbar_test.moc"
