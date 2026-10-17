// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// File Explorer's views under Qt Quick Test (tst_*.qml next to this one), on
// a real FolderModel over a folder of files made here: a recycled row or cell
// takes its new file's state at once (docs/visual-coherence.md §6.8). A
// private vela.conf sets the durations three times Vela's, so a fade still
// running would be caught.

#include "controls.h"
#include "foldermodel.h"
#include "iconprovider.h"

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QtQml/qqmlextensionplugin.h>
#include <QtQuickTest/quicktest.h>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

namespace {
QString folder;
}

class Setup : public QObject {
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        vela::controls::install(*engine, Appearance::Mode::Apps);
        engine->addImageProvider(QStringLiteral("fileicon"), new IconProvider(32));
        engine->rootContext()->setContextProperty(QStringLiteral("TestFolder"), folder);
    }
};

int main(int argc, char** argv)
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
    conf.close();

    // 300 files, sorted by name: file-000 … file-299.
    folder = home.filePath(QStringLiteral("files"));
    QDir().mkpath(folder);
    for (int i = 0; i < 300; ++i) {
        QFile file(QStringLiteral("%1/file-%2.txt").arg(folder).arg(i, 3, 10, QLatin1Char('0')));
        if (!file.open(QIODevice::WriteOnly)) {
            return 1;
        }
    }

    qmlRegisterType<FolderModel>("Vela.Files.Backend", 1, 0, "FolderModel");
    Setup setup;
    return quick_test_main_with_setup(argc, argv, "files-views", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "views_test.moc"
