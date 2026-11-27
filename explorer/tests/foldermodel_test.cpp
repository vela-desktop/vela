// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The icon a folder's entry shows (FolderModel::IconRole): the type's own
// icon when the theme has it, else its generic one, on a theme of the test's
// own (ctest clears QT_QPA_PLATFORMTHEME: a desktop's platform theme adds
// its own icons).

#include "foldermodel.h"

#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class FolderModelTest : public QObject {
    Q_OBJECT

    QTemporaryDir icons;

private slots:
    void initTestCase()
    {
        // A theme with image-png alone, in a size directory of its own.
        const QString theme = icons.filePath(QStringLiteral("vela-test"));
        QVERIFY(QDir().mkpath(theme + QStringLiteral("/16x16")));
        QFile index(theme + QStringLiteral("/index.theme"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("[Icon Theme]\nName=Vela test\nDirectories=16x16\n\n[16x16]\nSize=16\nType=Fixed\n");
        index.close();
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(theme + QStringLiteral("/16x16/image-png.png")));
        QIcon::setThemeSearchPaths({ icons.path() });
        QIcon::setThemeName(QStringLiteral("vela-test"));
        QIcon::setFallbackThemeName(QStringLiteral("vela-test")); // never the desktop's
        QVERIFY(QIcon::hasThemeIcon(QStringLiteral("image-png")));
        QVERIFY(!QIcon::hasThemeIcon(QStringLiteral("text-plain")));
    }

    void iconsFromTheTheme()
    {
        QTemporaryDir folder;
        for (const char* name : { "a.png", "b.png", "notes.txt" }) {
            QFile file(folder.filePath(QLatin1String(name)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QVERIFY(QDir(folder.path()).mkdir(QStringLiteral("inside")));

        FolderModel model;
        QSignalSpy loading(&model, &FolderModel::loadingChanged);
        model.setPath(folder.path());
        QTRY_VERIFY(!model.loading() && model.count() == 4);
        const auto icon = [&](const QString& name) {
            const int row = model.indexOf(folder.filePath(name));
            return row < 0 ? QStringLiteral("<missing>")
                           : model.data(model.index(row), FolderModel::IconRole).toString();
        };
        QCOMPARE(icon(QStringLiteral("a.png")), QStringLiteral("image-png")); // the theme has it
        QCOMPARE(icon(QStringLiteral("b.png")), QStringLiteral("image-png")); // remembered
        QCOMPARE(icon(QStringLiteral("notes.txt")), QStringLiteral("text-x-generic")); // only the generic
        QCOMPARE(icon(QStringLiteral("inside")), QStringLiteral("folder")); // no generic to fall back on
    }
};

QTEST_MAIN(FolderModelTest)
#include "foldermodel_test.moc"
