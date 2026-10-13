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
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>

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

    void tryExecResolvesBuildTree()
    {
        // AppModel uses the process executable directory to find sibling tools.
        // Run this test from a synthetic build/shell instead of relying on
        // whatever was built or installed beside the real test executable.
        if (qEnvironmentVariableIsSet("VELA_TEST_TRYEXEC_CHILD")) {
            QVERIFY(writeDesktop(userApplications() + QStringLiteral("/local-files.desktop"),
                "[Desktop Entry]\nType=Application\nName=Local Files\n"
                "Exec=vela-files\nTryExec=vela-files\n"));
            QVERIFY(writeDesktop(userApplications() + QStringLiteral("/local-settings.desktop"),
                "[Desktop Entry]\nType=Application\nName=Local Settings\n"
                "Exec=vela-settings\nTryExec=vela-settings\n"));
            QVERIFY(writeDesktop(userApplications() + QStringLiteral("/missing.desktop"),
                "[Desktop Entry]\nType=Application\nName=Missing Tool\n"
                "Exec=vela-files\nTryExec=not-installed-test-tool\n"));

            AppModel apps;
            apps.reload();
            QCOMPARE(apps.name(QStringLiteral("local-files.desktop")), QStringLiteral("Local Files"));
            QCOMPARE(apps.name(QStringLiteral("local-settings.desktop")), QStringLiteral("Local Settings"));
            QVERIFY(apps.name(QStringLiteral("missing.desktop")).isEmpty());
            return;
        }

        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());
        const QString shellDir = sandbox.filePath(QStringLiteral("build/shell"));
        const QString files = sandbox.filePath(QStringLiteral("build/explorer/vela-files"));
        const QString settings = sandbox.filePath(QStringLiteral("build/settings/vela-settings"));
        const QString runner = shellDir + u'/' + QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        QVERIFY(QDir().mkpath(shellDir));
        QVERIFY(writeDesktop(files, "#!/bin/sh\n"));
        QVERIFY(writeDesktop(settings, "#!/bin/sh\n"));
        const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
        QVERIFY(QFile::setPermissions(files, permissions));
        QVERIFY(QFile::setPermissions(settings, permissions));
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), runner));
        QVERIFY(QFile::setPermissions(runner, permissions));
        const QString emptyPath = sandbox.filePath(QStringLiteral("empty-bin"));
        QVERIFY(QDir().mkpath(emptyPath));

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PATH"), emptyPath);
        env.insert(QStringLiteral("VELA_TEST_TRYEXEC_CHILD"), QStringLiteral("1"));
        QProcess child;
        child.setProgram(runner);
        child.setArguments({ QStringLiteral("tryExecResolvesBuildTree") });
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start();
        QVERIFY2(child.waitForStarted(10000), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(20000), qPrintable(child.errorString()));
        const QByteArray output = child.readAll();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData());
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

    void symlinkedRootReplacementReattachesNestedWatches()
    {
        const QString link = userApplications();
        const QString parent = QFileInfo(link).absolutePath();
        const QString oldRoot = parent + QStringLiteral("/catalog-old");
        const QString newRoot = parent + QStringLiteral("/catalog-new");
        const QString oldApp = oldRoot + QStringLiteral("/nested/original.desktop");
        const QString newApp = newRoot + QStringLiteral("/nested/replacement.desktop");
        QVERIFY(writeDesktop(oldApp, entry("Old Catalog")));
        QVERIFY(writeDesktop(newApp, entry("New Catalog")));
        QVERIFY(QFile::link(oldRoot, link));
        QVERIFY(QFileInfo(link).isSymLink());

        AppModel apps;
        apps.reload();
        QCOMPARE(apps.name(QStringLiteral("nested-original.desktop")), QStringLiteral("Old Catalog"));
        QVERIFY(apps.name(QStringLiteral("nested-replacement.desktop")).isEmpty());

        // A package manager can atomically replace a symlink without changing
        // its pathname. rename(2) replaces the old symlink on Linux.
        const QString nextLink = link + QStringLiteral(".next");
        QVERIFY(QFile::link(newRoot, nextLink));
        const QByteArray from = QFile::encodeName(nextLink);
        const QByteArray to = QFile::encodeName(link);
        QCOMPARE(std::rename(from.constData(), to.constData()), 0);
        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("nested-replacement.desktop")),
            QStringLiteral("New Catalog"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(apps.name(QStringLiteral("nested-original.desktop")).isEmpty(), 5000);

        // No parent-directory event is generated for a file added inside an
        // existing nested directory: its own watcher must be reattached too.
        const QString later = newRoot + QStringLiteral("/nested/later.desktop");
        QVERIFY(writeDesktop(later, entry("Later Catalog")));
        QTRY_COMPARE_WITH_TIMEOUT(apps.name(QStringLiteral("nested-later.desktop")),
            QStringLiteral("Later Catalog"), 5000);
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
