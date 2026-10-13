// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// A nested build must not silently launch older /usr/bin Vela applications.

#include "velatool.h"
#include "desktopexec.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

class VelaToolTest : public QObject {
    Q_OBJECT

    QByteArray m_originalPath;
    bool m_hadPath = false;

    static void makeFile(const QString& path, bool executable = true)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("#!/bin/sh\n") > 0);
        file.close();
        QFileDevice::Permissions permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        if (executable)
            permissions |= QFileDevice::ExeOwner;
        QVERIFY(file.setPermissions(permissions));
    }

private slots:
    void init()
    {
        m_hadPath = qEnvironmentVariableIsSet("PATH");
        m_originalPath = qgetenv("PATH");
    }

    void cleanup()
    {
        if (m_hadPath)
            qputenv("PATH", m_originalPath);
        else
            qunsetenv("PATH");
    }

    void prefersBuildTree()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString shell = dir.filePath("build/shell");
        const QString settings = dir.filePath("build/settings/vela-settings");
        const QString files = dir.filePath("build/explorer/vela-files");
        const QString installed = dir.filePath("installed");
        QVERIFY(QDir().mkpath(shell)); // ../settings requires shell/ to exist
        makeFile(settings);
        makeFile(files);
        makeFile(installed + "/vela-settings");
        makeFile(installed + "/vela-files");
        qputenv("PATH", QFile::encodeName(installed));

        QCOMPARE(vela::toolExecutable("vela-settings", "settings", shell, "vela-shell"),
            QFileInfo(settings).canonicalFilePath());
        QCOMPARE(vela::toolExecutable("vela-files", "explorer", shell, "vela-shell"),
            QFileInfo(files).canonicalFilePath());
    }

    void besideExecutableThenPath()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString shell = dir.filePath("build/shell");
        const QString alongside = shell + "/vela-settings";
        const QString installed = dir.filePath("installed/vela-settings");
        makeFile(alongside);
        makeFile(installed);
        qputenv("PATH", QFile::encodeName(QFileInfo(installed).absolutePath()));

        QCOMPARE(vela::toolExecutable("vela-settings", "settings", shell, "vela-shell"),
            QFileInfo(alongside).canonicalFilePath());
        QVERIFY(QFile::remove(alongside));
        QCOMPARE(vela::toolExecutable("vela-settings", "settings", shell, "vela-shell"),
            QFileInfo(installed).canonicalFilePath());
        QVERIFY(QFile::remove(installed));
        QVERIFY(vela::toolExecutable("vela-settings", "settings", shell, "vela-shell").isEmpty());
    }

    void skipsNonExecutableBuildArtifacts()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString shell = dir.filePath("build/shell");
        const QString local = dir.filePath("build/settings/vela-settings");
        const QString installed = dir.filePath("installed/vela-settings");
        QVERIFY(QDir().mkpath(shell));
        makeFile(local, false);
        makeFile(installed);
        qputenv("PATH", QFile::encodeName(QFileInfo(installed).absolutePath()));

        QCOMPARE(vela::toolExecutable("vela-settings", "settings", shell, "vela-shell"),
            QFileInfo(installed).canonicalFilePath());
    }

    void desktopFileLaunches()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString shell = dir.filePath("build/shell");
        const QString files = dir.filePath("build/explorer/vela-files");
        const QString settings = dir.filePath("build/settings/vela-settings");
        const QString installedDir = dir.filePath("installed");
        const QString installedSettings = installedDir + QStringLiteral("/vela-settings");
        QVERIFY(QDir().mkpath(shell));
        makeFile(files);
        makeFile(settings);
        makeFile(installedDir + QStringLiteral("/vela-files"));
        makeFile(installedSettings);
        qputenv("PATH", QFile::encodeName(installedDir));

        // The same .desktop Exec parsing/expansion used by AppModel::launchEntry.
        const auto command = [&shell](const QString& exec, const QList<QUrl>& urls) {
            const auto parsed = DesktopExec::split(exec);
            QStringList args = DesktopExec::expand(parsed.arguments, urls, {});
            args[0] = vela::desktopExecutable(args.first(), shell);
            return args;
        };
        const QUrl url = QUrl::fromLocalFile(QStringLiteral("/tmp/file with space"));
        QCOMPARE(command(QStringLiteral("vela-files %U"), { url }),
            (QStringList { QFileInfo(files).canonicalFilePath(), url.toString(QUrl::FullyEncoded) }));
        QCOMPARE(command(QStringLiteral("vela-settings --page sound"), {}),
            (QStringList { QFileInfo(settings).canonicalFilePath(), QStringLiteral("--page"), QStringLiteral("sound") }));

        // Other applications, explicitly chosen paths, and field codes are untouched.
        QCOMPARE(command(QStringLiteral("other-app %f"), { url }),
            (QStringList { QStringLiteral("other-app"), url.toLocalFile() }));
        QCOMPARE(vela::desktopExecutable(QStringLiteral("/usr/bin/vela-files"), shell),
            QStringLiteral("/usr/bin/vela-files"));
        QCOMPARE(vela::desktopExecutable(QStringLiteral("vela-files-wrapper"), shell),
            QStringLiteral("vela-files-wrapper"));

        // Desktop actions can intentionally open a second instance of Files.
        QCOMPARE(vela::desktopExecutable(QStringLiteral("vela-files"), dir.filePath("build/explorer")),
            QFileInfo(files).canonicalFilePath());

        // Falling back to the installed program preserves existing launches.
        QVERIFY(QFile::remove(settings));
        QCOMPARE(vela::desktopExecutable(QStringLiteral("vela-settings"), shell),
            QFileInfo(installedSettings).canonicalFilePath());
        QVERIFY(QFile::remove(installedSettings));
        QCOMPARE(vela::desktopExecutable(QStringLiteral("vela-settings"), shell),
            QStringLiteral("vela-settings"));
    }

    void neverLaunchSelf()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString shell = dir.filePath("build/shell");
        QVERIFY(QDir().mkpath(shell));
        makeFile(dir.filePath("build/settings/vela-settings"));
        makeFile(dir.filePath("installed/vela-settings"));
        qputenv("PATH", QFile::encodeName(dir.filePath("installed")));

        QVERIFY(vela::toolExecutable("vela-settings", "settings", shell, "vela-settings").isEmpty());
    }
};

QTEST_GUILESS_MAIN(VelaToolTest)
#include "velatool.moc"
