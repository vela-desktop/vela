// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' Preferences (src/preferences.h), Personalization's choices that
// live in vela-shell.conf: what they read, the keys they write, when they
// say they changed. Everything is in a directory of its own (the XDG
// directories, the runtime directory with no compositor's socket in it), and
// only setters that write vela-shell.conf are called: none starts a process
// or talks to a compositor.

#include "preferences.h"
#include "privatehome.h"

#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QProcess>
#include <QSettings>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace vela::test;

class PreferencesTest : public QObject {
    Q_OBJECT

    static QVariant saved(const QString& key)
    {
        QSettings settings(confPath(), QSettings::IniFormat);
        return settings.value(key);
    }
    static void write(const QByteArray& text)
    {
        QVERIFY(writeFile(QStringLiteral("config/Vela/vela-shell.conf"), text));
    }
    static QString makeFile(const QString& name)
    {
        const QString path = home().filePath(name);
        paint(path, Qt::red);
        return path;
    }

    // Values written before the config path was blocked must stay put.
    static void expectRejectedChanges(Preferences& prefs)
    {
        prefs.setNightTemperature(3000);
        QCOMPARE(prefs.nightTemperature(), 4500);
        prefs.setTearing(true);
        QVERIFY(!prefs.tearing());
        prefs.setVrr(QStringLiteral("always"));
        QCOMPARE(prefs.vrr(), QStringLiteral("never"));
        prefs.setDirectScanout(QStringLiteral("auto"));
        QCOMPARE(prefs.directScanout(), QStringLiteral("no"));
        prefs.setMagnifierStep(200);
        QCOMPARE(prefs.magnifierStep(), 150);
        prefs.setColorFilter(false);
        QVERIFY(prefs.colorFilter());
        prefs.setStickyKeys(false);
        QVERIFY(prefs.stickyKeys());
        prefs.setAnimationEffects(true);
        QVERIFY(!prefs.animationEffects());
        prefs.setMouseSpeed(3);
        QCOMPARE(prefs.mouseSpeed(), 12);
        prefs.setTouchpad(true);
        QVERIFY(!prefs.touchpad());
    }

private slots:
    void duplicateCredentialBackendUsesTheLastEntry()
    {
        QVERIFY(writeFile(QStringLiteral("config/vela/vela.conf"),
            "# before\nsecret-backend=kwallet\nlanguage=it\nsecret-backend=gnome\n# after\n"));
        Preferences prefs;
        QCOMPARE(prefs.secretBackend(), QStringLiteral("gnome"));
        QVERIFY(prefs.secretBackendConfigured());
        prefs.setSecretBackend(QStringLiteral("kwallet"));
        QCOMPARE(prefs.saveError(), QString());
        QFile file(home().filePath(QStringLiteral("config/vela/vela.conf")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray text = file.readAll();
        QCOMPARE(text.count("secret-backend="), 1);
        QVERIFY(text.contains("secret-backend=kwallet\n"));
        QVERIFY(text.contains("language=it\n") && text.contains("# after"));
        Preferences reloaded;
        QCOMPARE(reloaded.secretBackend(), QStringLiteral("kwallet"));
    }

    /** Missing desktop helpers must produce a diagnostic without opening another app. */

    /** An executable whose interpreter is unavailable must report its launch failure. */

    /** The external opener receives the requested surface directory as one argument. */

    void init()
    {
        QDir().mkpath(QFileInfo(confPath()).absolutePath());
        // The apps' mode set: otherwise Preferences asks gsettings.
        write("[appearance]\nappTheme=dark\n");
    }



    // docs/battery-notifications.md §6.




    void compositorSaveFailureRestoresTheValue()
    {
        // A file where the config directory should be: the write cannot commit.
        const QString blocker = home().filePath(QStringLiteral("config/vela"));
        QDir(blocker).removeRecursively();
        QVERIFY(writeFile(QStringLiteral("config/vela"), "not a directory\n"));
        Preferences prefs;
        QCOMPARE(prefs.screenOffMinutes(), 10);
        QCOMPARE(prefs.saveError(), QString());
        QSignalSpy idle(&prefs, &Preferences::idleChanged);
        QSignalSpy error(&prefs, &Preferences::saveErrorChanged);
        prefs.setScreenOffMinutes(3);
        QCOMPARE(prefs.screenOffMinutes(), 10);
        QVERIFY(prefs.saveError().contains(home().filePath(QStringLiteral("config/vela/vela.conf"))));
        QCOMPARE(idle.count(), 1);
        QCOMPARE(error.count(), 1);

        QVERIFY(QFile::remove(blocker));
        QVERIFY(QDir().mkpath(home().filePath(QStringLiteral("config/vela"))));
        prefs.setScreenOffMinutes(3);
        QCOMPARE(prefs.screenOffMinutes(), 3);
        QCOMPARE(prefs.saveError(), QString());
        QFile file(home().filePath(QStringLiteral("config/vela/vela.conf")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(file.readAll()).contains(QStringLiteral("screen-off=3")));
    }

    void compositorSettingsRoundTrip()
    {
        const QString dir = home().filePath(QStringLiteral("config/vela"));
        QDir(dir).removeRecursively();
        QVERIFY(QDir().mkpath(dir));
        Preferences prefs;
        prefs.setScreenOffMinutes(4);
        prefs.setLockOnIdle(false);
        prefs.setLanguage(QStringLiteral("it"));
        prefs.addKeyboardLayout(QStringLiteral("us"), QString());
        prefs.addKeyboardLayout(QStringLiteral("it"), QStringLiteral("nodeadkeys"));
        prefs.moveKeyboardLayoutUp(1);
        prefs.removeKeyboardLayout(0);
        prefs.setRepeatDelay(500);
        prefs.setRepeatRate(40);
        prefs.setNightLight(true);
        prefs.setNightTemperature(4500);
        prefs.setNightNvidiaFix(true);
        prefs.setNightSchedule(QStringLiteral("yes"));
        prefs.setNightFrom(QStringLiteral("20:15"));
        prefs.setNightTo(QStringLiteral("06:45"));
        prefs.setTearing(false);
        prefs.setVrr(QStringLiteral("never"));
        prefs.setDirectScanout(QStringLiteral("no"));
        prefs.setMagnifierStep(150);
        prefs.setColorFilter(true);
        prefs.setColorFilterKind(QStringLiteral("inverted"));
        prefs.setColorFilterShortcut(true);
        prefs.setStickyKeys(true);
        prefs.setAnimationEffects(false);
        prefs.setAnimationDuration(80);
        prefs.setMouseSpeed(12);
        prefs.setMousePrecision(false);
        prefs.setMouseLeftHanded(true);
        prefs.setWheelLines(5);
        prefs.setTouchpad(false);
        prefs.setTouchpadWithMouse(false);
        prefs.setTouchpadSpeed(12);
        prefs.setTouchpadTap(false);
        prefs.setTouchpadNatural(false);
        prefs.setThreeFingers(QStringLiteral("desktop"));
        prefs.setFourFingers(QStringLiteral("none"));
        QCOMPARE(prefs.saveError(), QString());
        QCOMPARE(prefs.screenOffMinutes(), 4);
        QCOMPARE(prefs.keyboardLayouts().size(), 1);
        QCOMPARE(prefs.nightLight(), true);
        QCOMPARE(prefs.directScanout(), QStringLiteral("no"));
        QFile conf(dir + QStringLiteral("/vela.conf"));
        QVERIFY(conf.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(conf.readAll());
        conf.close();
        QVERIFY(text.contains(QStringLiteral("language=it")));
        QVERIFY(text.contains(QStringLiteral("keyboard-layout=us")));
        QVERIFY(text.contains(QStringLiteral("night-light=yes")));
        QVERIFY(text.contains(QStringLiteral("mouse-primary-button=right")));

        {
            // An immutable file still reads, but QSaveFile cannot replace it.
            // That is the reload path. chattr is optional; the file-blocker below
            // covers the same setters when the file cannot be reread.
            const bool frozen = geteuid() == 0
                && QProcess::execute(QStringLiteral("chattr"), { QStringLiteral("+i"), conf.fileName() }) == 0;
            const auto thaw = qScopeGuard([frozen, path = conf.fileName()] {
                if (frozen) {
                    QProcess::execute(QStringLiteral("chattr"), { QStringLiteral("-i"), path });
                }
            });
            if (frozen) {
                prefs.setScreenOffMinutes(9);
                QCOMPARE(prefs.screenOffMinutes(), 4);
                QVERIFY(!prefs.saveError().isEmpty());
                prefs.setLockOnIdle(true);
                QVERIFY(!prefs.lockOnIdle());
                prefs.setLanguage(QStringLiteral("en"));
                QCOMPARE(prefs.language(), QStringLiteral("it"));
                prefs.setNightLight(false);
                QVERIFY(prefs.nightLight());
                expectRejectedChanges(prefs);
            }
        }

        // A file where the config directory should be. Root can still write
        // into a mode-0555 directory, which is what the CI runners are, so
        // the block has to be the path itself.
        const QString saved = text;
        QVERIFY(QDir(dir).removeRecursively());
        QVERIFY(writeFile(QStringLiteral("config/vela"), "not a directory\n"));
        prefs.setScreenOffMinutes(9);
        QCOMPARE(prefs.screenOffMinutes(), 4);
        QVERIFY(!prefs.saveError().isEmpty());
        prefs.setNightLight(false);
        QVERIFY(prefs.nightLight());
        prefs.setLanguage(QStringLiteral("en"));
        QCOMPARE(prefs.language(), QStringLiteral("it"));
        prefs.addKeyboardLayout(QStringLiteral("de"), QString());
        QCOMPARE(prefs.keyboardLayouts().size(), 1);
        prefs.setLockOnIdle(true);
        QVERIFY(!prefs.lockOnIdle());
        expectRejectedChanges(prefs);

        QVERIFY(QFile::remove(dir));
        QVERIFY(QDir().mkpath(dir));
        QVERIFY(writeFile(QStringLiteral("config/vela/vela.conf"), saved.toUtf8()));
        prefs.setLanguage(QStringLiteral("en"));
        QCOMPARE(prefs.language(), QStringLiteral("en"));
        QCOMPARE(prefs.saveError(), QString());
    }
};

int main(int argc, char** argv)
{
    // A distribution whose logo is a loose file in pixmaps/.
    if (!isolate()
        || !writeFile(QStringLiteral("os-release"), "NAME=Test\nPRETTY_NAME=\"Test Linux 2\"\nLOGO=test-logo\n")
        || !writeFile(QStringLiteral("data/pixmaps/test-logo.png"), "png")) {
        return 1;
    }

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-settings"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    PreferencesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "preferences_test.moc"
