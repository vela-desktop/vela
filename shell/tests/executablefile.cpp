// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../common/executablefile.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

class ExecutableFileTest : public QObject {
    Q_OBJECT

private slots:
    void classify_data();
    void classify();
    void missingExecutablePermission();
};

void ExecutableFileTest::classify_data()
{
    QTest::addColumn<QString>("filename");
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<bool>("expected");

    const QByteArray elf("\x7f" "ELF" "payload", 11);
    QTest::newRow("AppImage") << QStringLiteral("my-app.AppImage") << elf << true;
    QTest::newRow("ELF-without-extension") << QStringLiteral("program") << elf << true;
    QTest::newRow("ELF-software-name") << QStringLiteral("my.software.AppImage") << elf << true;
    QTest::newRow("ELF-so-in-middle") << QStringLiteral("app.so.backup.AppImage") << elf << true;
    QTest::newRow("unversioned-library") << QStringLiteral("libexample.so") << elf << false;
    QTest::newRow("versioned-library") << QStringLiteral("libexample.so.6") << elf << false;
    QTest::newRow("multi-version-library") << QStringLiteral("libexample.so.6.1.2") << elf << false;
    QTest::newRow("shell-script") << QStringLiteral("run.sh") << QByteArray("#!/bin/sh\n") << false;
    QTest::newRow("document") << QStringLiteral("file.pdf") << QByteArray("%PDF-1.7") << false;
    QTest::newRow("desktop-launcher") << QStringLiteral("program.desktop") << QByteArray("[Desktop Entry]\n") << false;
    QTest::newRow("truncated-ELF") << QStringLiteral("truncated") << QByteArray("\x7f" "EL", 3) << false;
}

void ExecutableFileTest::classify()
{
    QFETCH(QString, filename);
    QFETCH(QByteArray, contents);
    QFETCH(bool, expected);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath(filename));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), qint64(contents.size()));
    file.close();
    QVERIFY(QFile::setPermissions(file.fileName(), QFileDevice::ReadOwner
        | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QCOMPARE(vela::files::isProgram(QFileInfo(file.fileName())), expected);
}

void ExecutableFileTest::missingExecutablePermission()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath(QStringLiteral("download.AppImage")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray elf("\x7f" "ELF", 4);
    QCOMPARE(file.write(elf), qint64(elf.size()));
    file.close();

    QVERIFY(QFile::setPermissions(file.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const QFileInfo withoutPermission(file.fileName());
    QVERIFY(vela::files::isProgram(withoutPermission));
    QVERIFY(!withoutPermission.isExecutable());

    QVERIFY(QFile::setPermissions(file.fileName(), QFileDevice::ReadOwner
        | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    const QFileInfo withPermission(file.fileName());
    QVERIFY(vela::files::isProgram(withPermission));
    QVERIFY(withPermission.isExecutable());
}

QTEST_GUILESS_MAIN(ExecutableFileTest)
#include "executablefile.moc"
