// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// A nested build must not silently launch older /usr/bin Vela applications.

#include "velatool.h"

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
        makeFile(local, false);
        makeFile(installed);
        qputenv("PATH", QFile::encodeName(QFileInfo(installed).absolutePath()));

        QCOMPARE(vela::toolExecutable("vela-settings", "settings", shell, "vela-shell"),
            QFileInfo(installed).canonicalFilePath());
    }

    void neverLaunchSelf()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString shell = dir.filePath("build/shell");
        makeFile(dir.filePath("build/settings/vela-settings"));
        makeFile(dir.filePath("installed/vela-settings"));
        qputenv("PATH", QFile::encodeName(dir.filePath("installed")));

        QVERIFY(vela::toolExecutable("vela-settings", "settings", shell, "vela-settings").isEmpty());
    }
};

QTEST_GUILESS_MAIN(VelaToolTest)
#include "velatool.moc"
