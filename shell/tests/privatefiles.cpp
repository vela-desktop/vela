// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../common/privatefile.h"
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>

class PrivateFiles : public QObject {
    Q_OBJECT

private slots:
    void createsPrivateFile();
    void replacesLegacyPermissiveFile();
};

static mode_t modeOf(const QString& path)
{
    struct stat info {};
    return stat(QFile::encodeName(path).constData(), &info) == 0 ? (info.st_mode & 0777) : 0777;
}

void PrivateFiles::createsPrivateFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("new.png"));
    const mode_t before = umask(0);
    QSaveFile file(path);
    const bool opened = vela::openPrivateFile(file);
    umask(before);
    QVERIFY(opened);
    QCOMPARE(file.write("png-data"), qint64(8));
    QVERIFY(file.commit());
    QCOMPARE(modeOf(path), mode_t(0600));
}

void PrivateFiles::replacesLegacyPermissiveFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vela.conf"));
    QFile original(path);
    QVERIFY(original.open(QIODevice::WriteOnly));
    QCOMPARE(original.write("old"), qint64(3));
    original.close();
    QVERIFY(original.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QCOMPARE(modeOf(path), mode_t(0644));
    QSaveFile replacement(path);
    QVERIFY(vela::openPrivateFile(replacement, QIODevice::WriteOnly | QIODevice::Text));
    QCOMPARE(replacement.write("new"), qint64(3));
    QVERIFY(replacement.commit());
    QCOMPARE(modeOf(path), mode_t(0600));
    QFile result(path);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.readAll(), QByteArray("new"));
}

QTEST_GUILESS_MAIN(PrivateFiles)
#include "privatefiles.moc"
