// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Real filesystem notifications, no compositor or Wayland session required.
// New, changed and removed .desktop files must appear in AppModel without
// manually calling reload() after its initial scan.

#include "appmodel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

QString userApplications()
{
    return qEnvironmentVariable("XDG_DATA_HOME") + QStringLiteral("/applications");
}

QString systemApplications()
{
    return qEnvironmentVariable("VELA_TEST_SYSTEM_DATA") + QStringLiteral("/applications");
}

QString flatpakApplications()
{
    return qEnvironmentVariable("VELA_TEST_FLATPAK_DATA") + QStringLiteral("/applications");
}

bool writeDesktop(const QString& path, const QByteArray& text)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(text) == text.size();
}

QByteArray entry(const QByteArray& name, const QByteArray& extra = {})
{
    return "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=true\n" + extra;
}

} // namespace

class AppModelWatchTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        // Each model from the previous test has already been destroyed.
        QDir(userApplications()).removeRecursively();
        QDir(systemApplications()).removeRecursively();
        QDir(flatpakApplications()).removeRecursively();
    }

    void installsAndUninstallsWithoutRestart()
    {
        // The per-user applications directory doesn't exist at startup.
        AppModel apps;
        apps.reload();
        apps.setQuery(QStringLiteral("Example Player"));
        QSignalSpy countSignals(&apps, &AppModel::countChanged);

        const QString path = userApplications() + QStringLiteral("/example-player.desktop");
        QVERIFY(writeDesktop(path, entry("Example Player")));

        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("example-player.desktop")),
            QStringLiteral("Example Player"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(apps.count(), 1, 5000);

        const qsizetype signalsBeforeRemove = countSignals.size();
        QVERIFY(QFile::remove(path));
        QTRY_VERIFY_WITH_TIMEOUT(apps.name(QStringLiteral("example-player.desktop")).isEmpty(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(apps.count(), 0, 5000);
        QVERIFY(countSignals.size() > signalsBeforeRemove);
    }

    void editsInPlaceUpdateNamesAndVisibility()
    {
        const QString path = userApplications() + QStringLiteral("/edited.desktop");
        QVERIFY(writeDesktop(path, entry("Original Name")));

        AppModel apps;
        apps.reload();
        QCOMPARE(apps.name(QStringLiteral("edited.desktop")), QStringLiteral("Original Name"));
        QSignalSpy revisions(&apps, &AppModel::catalogRevisionChanged);
        const int revisionBeforeEdit = apps.catalogRevision();

        // Truncating/re-writing the same inode need not change the directory.
        QVERIFY(writeDesktop(path, entry("Updated Name")));
        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("edited.desktop")),
            QStringLiteral("Updated Name"), 5000);
        QVERIFY(apps.catalogRevision() > revisionBeforeEdit);
        QVERIFY(!revisions.isEmpty());

        QVERIFY(writeDesktop(path, entry("Hidden Name", "NoDisplay=true\n")));
        QTRY_VERIFY_WITH_TIMEOUT(apps.name(QStringLiteral("edited.desktop")).isEmpty(), 5000);

        QVERIFY(writeDesktop(path, entry("Visible Again")));
        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("edited.desktop")),
            QStringLiteral("Visible Again"), 5000);
    }

    void reloadResetsOnceAndReportsLastAppRemoval()
    {
        const QString path = userApplications() + QStringLiteral("/reset.desktop");
        QVERIFY(writeDesktop(path, entry("Reset Example")));
        AppModel apps;
        apps.reload();
        apps.setQuery(QStringLiteral("Reset"));
        QCOMPARE(apps.count(), 1);

        QSignalSpy resets(&apps, &QAbstractItemModel::modelReset);
        QSignalSpy countSignals(&apps, &AppModel::countChanged);
        QSignalSpy revisions(&apps, &AppModel::catalogRevisionChanged);

        QVERIFY(QFile::remove(path));
        apps.reload();
        QCOMPARE(apps.count(), 0);
        QCOMPARE(resets.size(), 1);
        QCOMPARE(countSignals.size(), 1);
        QCOMPARE(revisions.size(), 1);
    }

    void nestedDirectoriesAreDiscovered()
    {
        QVERIFY(QDir().mkpath(userApplications()));
        AppModel apps;
        apps.reload();

        const QString path = userApplications() + QStringLiteral("/vendor/tools/nested.desktop");
        QVERIFY(writeDesktop(path, entry("Nested Tool")));

        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("vendor-tools-nested.desktop")),
            QStringLiteral("Nested Tool"), 5000);

        const QString another = userApplications() + QStringLiteral("/vendor/tools/another.desktop");
        QVERIFY(writeDesktop(another, entry("Another Tool")));
        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("vendor-tools-another.desktop")),
            QStringLiteral("Another Tool"), 5000);
    }

    void systemAndFlatpakExportsUpdate()
    {
        const QString systemPath = systemApplications() + QStringLiteral("/override.desktop");
        QVERIFY(writeDesktop(systemPath, entry("System Version")));

        AppModel apps;
        apps.reload();
        QCOMPARE(apps.name(QStringLiteral("override.desktop")), QStringLiteral("System Version"));

        // Local .desktop files override system entries with the same ID.
        const QString localPath = userApplications() + QStringLiteral("/override.desktop");
        QVERIFY(writeDesktop(localPath, entry("User Version")));
        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("override.desktop")),
            QStringLiteral("User Version"), 5000);

        QVERIFY(QFile::remove(localPath));
        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("override.desktop")),
            QStringLiteral("System Version"), 5000);

        // A Flatpak export root can be created after the first scan.
        const QString flatpakPath = flatpakApplications() + QStringLiteral("/org.example.Flatpak.desktop");
        QVERIFY(writeDesktop(flatpakPath, entry("Flatpak Example")));
        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("org.example.Flatpak.desktop")),
            QStringLiteral("Flatpak Example"), 5000);
    }
};

int main(int argc, char* argv[])
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) {
        return 1;
    }

    const QString userData = sandbox.filePath(QStringLiteral("user"));
    const QString systemData = sandbox.filePath(QStringLiteral("system"));
    const QString flatpakData = sandbox.filePath(QStringLiteral("flatpak/exports/share"));

    qputenv("XDG_DATA_HOME", QFile::encodeName(userData));
    qputenv("XDG_DATA_DIRS", QFile::encodeName(systemData) + ':' + QFile::encodeName(flatpakData));
    qputenv("VELA_TEST_SYSTEM_DATA", QFile::encodeName(systemData));
    qputenv("VELA_TEST_FLATPAK_DATA", QFile::encodeName(flatpakData));

    QCoreApplication app(argc, argv);
    AppModelWatchTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "appmodel_watch.moc"
