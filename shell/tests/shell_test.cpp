// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's own QML pieces under Qt Quick Test (the tst_*.qml files next
// to this one), as Vela.Controls' runner does, plus what the shell's
// windows find in their context: a fake Vela.Bluetooth
// (common/bluetooth/tests/FakeBluetooth.qml), `Shell` (FakeShell.qml),
// `BluetoothPrompts` (FakeBluetoothPrompts.qml) and `Sound`
// (FakeSound.qml), and clipboard history's images (FakeClipboardImages).
// A private vela.conf sets
// the durations three times Vela's; with VELA_TEST_ANIMATION_EFFECTS=no it
// turns animation effects off (two ctest entries).

#include "accessibleprobe.h"
#include "controls.h"
#include "quicktest.h"

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QtQml/qqmlextensionplugin.h>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)
Q_IMPORT_QML_PLUGIN(Vela_Bluetooth_UiPlugin)

// image://clipboard/<id> (ClipboardList.qml): the same grey picture for
// every entry, where the shell's ClipboardImageProvider gives each its own.
class FakeClipboardImages : public QQuickImageProvider {
public:
    FakeClipboardImages()
        : QQuickImageProvider(QQuickImageProvider::Image)
    {
    }

    QImage requestImage(const QString&, QSize* size, const QSize&) override
    {
        QImage image(160, 90, QImage::Format_RGB32);
        image.fill(Qt::gray);
        if (size) {
            *size = image.size();
        }
        return image;
    }
};

class Setup : public QObject {
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        // Controls set their accessible role and state only once a screen
        // reader is there.
        QAccessible::setActive(true);
        vela::test::AccessibleProbe::install();
        vela::controls::install(*engine, Appearance::Mode::Apps);
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(FAKE_BLUETOOTH)), "Vela.Bluetooth", 1, 0, "Bluetooth");
        // The shell's styles as the singleton they are in the shell: the
        // tests import its QML as a directory, where Styles.qml is a plain
        // type (`import Vela.Shell.Test as ShellTest`, ShellTest.Styles).
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(QUICK_TEST_SOURCE_DIR "/../qml/Styles.qml")),
            "Vela.Shell.Test", 1, 0, "Styles");
        engine->addImageProvider(QStringLiteral("clipboard"), new FakeClipboardImages);
        for (const auto& [name, file] : {
                 std::pair { "Shell", "FakeShell.qml" },
                 std::pair { "BluetoothPrompts", "FakeBluetoothPrompts.qml" },
                 std::pair { "Sound", "FakeSound.qml" },
             }) {
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
