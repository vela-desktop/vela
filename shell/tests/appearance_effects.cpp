// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Style.animationEffects (docs/visual-coherence.md §6.5): read from
// animation-effects in vela.conf, yes by default, and followed live when
// Settings replaces the file.

#include "appearance.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

QString velaConf()
{
    return qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vela/vela.conf");
}

// Like Settings: a new file renamed over the old one.
bool writeConf(const QByteArray& text)
{
    QDir().mkpath(qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vela"));
    QSaveFile file(velaConf());
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size() && file.commit();
}

} // namespace

class AppearanceEffectsTest : public QObject {
    Q_OBJECT

private slots:
    void init() { QFile::remove(velaConf()); }

    void onWithoutTheKey()
    {
        QVERIFY(writeConf("sticky-keys=no\n"));
        QVERIFY(Appearance().animationEffects());
    }

    void followsTheFile()
    {
        QVERIFY(writeConf("color-filters=no\nanimation-effects=no\n"));
        Appearance appearance;
        QVERIFY(!appearance.animationEffects());
        appearance.watch();
        QSignalSpy changed(&appearance, &Appearance::changed);
        QVERIFY(writeConf("color-filters=no\nanimation-effects=yes\n"));
        QTRY_VERIFY(appearance.animationEffects());
        QVERIFY(changed.count() >= 1);
        QVERIFY(writeConf("animation-effects=no\n"));
        QTRY_VERIFY(!appearance.animationEffects());
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    AppearanceEffectsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "appearance_effects.moc"
