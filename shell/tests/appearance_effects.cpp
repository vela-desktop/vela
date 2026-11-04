// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Style.animationEffects and Style.animationDuration (docs/visual-coherence.md
// §6.5): read from animation-effects and animation-duration in vela.conf (on,
// at Vela's durations, by default), and followed live when Settings replaces
// the file.

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
        const Appearance appearance;
        QVERIFY(appearance.animationEffects());
        QCOMPARE(appearance.animationDuration(), 1.0);
    }

    void durationWithinItsRange()
    {
        QVERIFY(writeConf("animation-duration=150\n"));
        QCOMPARE(Appearance().animationDuration(), 1.5);
        QVERIFY(writeConf("animation-duration=1000\n"));
        QCOMPARE(Appearance().animationDuration(), 3.0);
        QVERIFY(writeConf("animation-duration=10\n"));
        QCOMPARE(Appearance().animationDuration(), 0.5);
        QVERIFY(writeConf("animation-duration=slow\n"));
        QCOMPARE(Appearance().animationDuration(), 1.0);
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
        QVERIFY(writeConf("animation-effects=no\nanimation-duration=200\n"));
        QTRY_COMPARE(appearance.animationDuration(), 2.0);
    }

    // The user's themes directory created only after the watch began: its
    // theme shows once the file is there.
    void followsAThemesDirectoryCreatedLater()
    {
        const QString themes = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vela/themes");
        QDir(themes).removeRecursively();
        QVERIFY(writeConf("theme=fresh\n"));
        Appearance appearance;
        appearance.watch();
        QVERIFY(appearance.theme().isEmpty());
        QVERIFY(QDir().mkpath(themes));
        QTest::qWait(300);
        QSaveFile file(themes + QStringLiteral("/fresh.conf"));
        QVERIFY(file.open(QIODevice::WriteOnly) && file.write("radius=7\n") == 9 && file.commit());
        QTRY_COMPARE(appearance.theme().value(QStringLiteral("radius")).toDouble(), 7.0);
    }

    // Style.theme: theme= of vela.conf, then the theme file itself, live.
    void followsTheTheme()
    {
        const QString themes = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vela/themes");
        QDir().mkpath(themes);
        const auto writeTheme = [&](const QByteArray& text) {
            QSaveFile file(themes + QStringLiteral("/test.conf"));
            return file.open(QIODevice::WriteOnly) && file.write(text) == text.size() && file.commit();
        };
        QVERIFY(writeTheme("radius=6\n"));
        QVERIFY(writeConf("theme=test\n"));
        Appearance appearance;
        QCOMPARE(appearance.theme().value(QStringLiteral("radius")).toDouble(), 6.0);
        appearance.watch();
        QSignalSpy changed(&appearance, &Appearance::changed);
        QVERIFY(writeTheme("radius=10\n"));
        QTRY_COMPARE(appearance.theme().value(QStringLiteral("radius")).toDouble(), 10.0);
        QVERIFY(changed.count() >= 1);
        QVERIFY(writeConf("theme=\n"));
        QTRY_VERIFY(appearance.theme().isEmpty());
        QVERIFY(writeConf("theme=../vela\n")); // not a theme's name
        QTest::qWait(300);
        QVERIFY(appearance.theme().isEmpty());
        // A theme chosen before its file exists: it shows once the file is there.
        QVERIFY(writeConf("theme=later\n"));
        QTest::qWait(300);
        QVERIFY(appearance.theme().isEmpty());
        QSaveFile later(themes + QStringLiteral("/later.conf"));
        QVERIFY(later.open(QIODevice::WriteOnly) && later.write("radius=12\n") == 10 && later.commit());
        QTRY_COMPARE(appearance.theme().value(QStringLiteral("radius")).toDouble(), 12.0);
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
    // Themes are looked up in the system's directories too: none of the
    // machine's may count here.
    qputenv("XDG_CONFIG_DIRS", home.filePath(QStringLiteral("sys-config")).toUtf8());
    qputenv("XDG_DATA_HOME", home.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_DATA_DIRS", home.filePath(QStringLiteral("sys-data")).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    AppearanceEffectsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "appearance_effects.moc"
