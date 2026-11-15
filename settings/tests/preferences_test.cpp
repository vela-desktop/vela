// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' Preferences (src/preferences.h), the 0.1 shell choices that
// live in vela-shell.conf: what they read, the keys they write, when they
// say they changed. Everything is in a directory of its own (the XDG
// directories, the runtime directory with no compositor's socket in it), and
// appearance commands use private helpers. No host configuration or live
// compositor is used.

#include "defaults.h"
#include "controls.h"
#include "preferences.h"
#include "privatehome.h"

#include <unistd.h>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMetaProperty>
#include <QPalette>
#include <QTextStream>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QtQml/qqmlextensionplugin.h>
#include <QSettings>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

using namespace vela::test;

// A file in place of the configuration directory blocks writes even on
// root CI runners. Keep the saved directory for restart/recovery assertions.
class BlockedDirectory {
public:
    explicit BlockedDirectory(QString path = QFileInfo(confPath()).absolutePath())
        : m_path(std::move(path)), m_backup(m_path + QStringLiteral("-saved")) {}
    ~BlockedDirectory() { restore(); }
    bool block()
    {
        if (!QDir().rename(m_path, m_backup)) return false;
        m_blocked = true;
        QFile blocker(m_path);
        return blocker.open(QIODevice::WriteOnly) && blocker.write("blocked\n") == 8;
    }
    bool restore()
    {
        if (!m_blocked) return true;
        if (!QFile::remove(m_path) || !QDir().rename(m_backup, m_path)) return false;
        m_blocked = false;
        return true;
    }
private:
    QString m_path;
    QString m_backup;
    bool m_blocked = false;
};

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

    // A separate process cannot see QSettings' shared in-process cache.
    static void expectFreshShell(const QVariantMap& expected, const QStringList& absent = {})
    {
        QProcess reader;
        reader.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--read-shell") });
        QVERIFY(reader.waitForFinished(5000));
        QCOMPARE(reader.exitStatus(), QProcess::NormalExit);
        QCOMPARE(reader.exitCode(), 0);
        const QJsonObject values = QJsonDocument::fromJson(reader.readAllStandardOutput()).object();
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            QVERIFY(values.contains(it.key()));
            const QVariant actual = values.value(it.key()).toVariant();
            // INI keeps booleans and one-element string lists as strings;
            // apply the same type conversion as the consuming preferences.
            if (it.value().metaType().id() == QMetaType::Bool) {
                QCOMPARE(actual.toBool(), it.value().toBool());
            } else if (it.value().metaType().id() == QMetaType::QStringList) {
                QCOMPARE(actual.toStringList(), it.value().toStringList());
            } else {
                QCOMPARE(values.value(it.key()), QJsonValue::fromVariant(it.value()));
            }
        }
        for (const QString& key : absent) {
            QVERIFY(!values.contains(key));
        }
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


    void init()
    {
        QDir().mkpath(QFileInfo(confPath()).absolutePath());
        // The apps' mode set: otherwise Preferences asks gsettings.
        write("[appearance]\nappTheme=dark\n");
    }

    void shellWriteFailure_data()
    {
        QTest::addColumn<QByteArray>("property");
        QTest::addColumn<QString>("key");
        QTest::addColumn<QVariant>("value");
        QTest::addColumn<bool>("present");
        struct Choice { const char* property; const char* key; QVariant value; };
        const QList<Choice> choices {
            { "accent", "appearance/accent", QColor(Qt::red) },
            { "shellTheme", "appearance/shellTheme", QStringLiteral("light") },
            { "taskbarAlignment", "taskbar/alignment", QStringLiteral("left") },
            { "endTask", "taskbar/endTask", false },
            { "taskView", "taskbar/taskView", false },
            { "taskbarAllScreens", "taskbar/allScreens", false },
            { "showDesktopCorner", "taskbar/showDesktopCorner", false },
            { "doNotDisturb", "notifications/doNotDisturb", true },
            { "clipboardHistory", "clipboard/history", true }
        };
        for (const auto& choice : choices) {
            for (bool present : { false, true }) {
                const QByteArray name = QByteArray(choice.property) + (present ? "-existing" : "-absent");
                QTest::newRow(name.constData()) << QByteArray(choice.property) << QString::fromLatin1(choice.key)
                    << choice.value << present;
            }
        }
    }

    void shellWriteFailure()
    {
        QFETCH(QByteArray, property);
        QFETCH(QString, key);
        QFETCH(QVariant, value);
        QFETCH(bool, present);
        Preferences prefs;
        const QMetaProperty member = prefs.metaObject()->property(prefs.metaObject()->indexOfProperty(property));
        QVERIFY(member.isWritable());
        const QVariant original = member.read(&prefs);
        QVERIFY(original != value);
        if (present) {
            QSettings initial(confPath(), QSettings::IniFormat);
            initial.setValue(key, property == "clock24" ? QVariant(24) : original.toString());
            initial.sync();
            QCOMPARE(initial.status(), QSettings::NoError);
        }
        const QVariant previous = saved(key);
        QSignalSpy changed(&prefs, member.notifySignal());
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        QVERIFY(member.write(&prefs, value));
        QCOMPARE(member.read(&prefs), original);
        QVERIFY(prefs.saveError().contains(confPath()));
        QVERIFY(!prefs.saveError().contains(QStringLiteral("/vela/vela.conf")));
        QVERIFY(changed.count() >= 1); // re-evaluate a control even when the value stayed put
        QVERIFY(blocked.restore());
        // A different writer and deferred syncs must not resurrect the failed value.
        {
            QSettings other(confPath(), QSettings::IniFormat);
            other.setValue(QStringLiteral("test/probe"), true);
            other.sync();
            QCOMPARE(other.status(), QSettings::NoError);
            QCOMPARE(other.contains(key), present);
            QCOMPARE(other.value(key).toString(), previous.toString());
        }
        QCoreApplication::processEvents();
        prefs.reload();
        QCOMPARE(member.read(&prefs), original);
        changed.clear();
        QVERIFY(member.write(&prefs, value));
        QCOMPARE(member.read(&prefs), value);
        QCOMPARE(prefs.saveError(), QString());
        QCOMPARE(changed.count(), 1);
        Preferences reloaded;
        QCOMPARE(member.read(&reloaded), value);
    }

    void wallpaperFailureKeepsThePictureAndHistoryTogether()
    {
        const QString oldPicture = makeFile(QStringLiteral("saved-wallpaper.png"));
        const QString newPicture = makeFile(QStringLiteral("rejected-wallpaper.png"));
        Preferences prefs;
        prefs.setWallpaper(oldPicture);
        const QStringList recent = prefs.recentWallpapers();
        QSignalSpy changed(&prefs, &Preferences::wallpaperChanged);
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        prefs.setWallpaper(newPicture);
        QCOMPARE(prefs.wallpaper(), oldPicture);
        QCOMPARE(prefs.recentWallpapers(), recent);
        QVERIFY(changed.count() >= 1);
        QVERIFY(blocked.restore());
        prefs.setShowDesktopCorner(false);
        expectFreshShell({ { QStringLiteral("appearance/wallpaper"), oldPicture },
            { QStringLiteral("appearance/recentWallpapers"), recent },
            { QStringLiteral("taskbar/showDesktopCorner"), false } });
        prefs.reload();
        QCOMPARE(prefs.wallpaper(), oldPicture);
        prefs.setWallpaper(newPicture);
        QCOMPARE(prefs.wallpaper(), newPicture);
        QCOMPARE(prefs.recentWallpapers().first(), newPicture);
        QCOMPARE(prefs.saveError(), QString());
    }

    void shellAndCompositorErrorsStayIndependent_data()
    {
        QTest::addColumn<bool>("shellFirst");
        QTest::newRow("repair-shell-first") << true;
        QTest::newRow("repair-compositor-first") << false;
    }

    void shellAndCompositorErrorsStayIndependent()
    {
        QFETCH(bool, shellFirst);
        QVERIFY(writeFile(QStringLiteral("config/vela/vela.conf"), "screen-off=10\n"));
        Preferences prefs;
        BlockedDirectory shell;
        BlockedDirectory compositor(home().filePath(QStringLiteral("config/vela")));
        QVERIFY(shell.block());
        QVERIFY(compositor.block());
        prefs.setShowDesktopCorner(false);
        prefs.setScreenOffMinutes(3);
        QVERIFY(prefs.saveError().contains(confPath()));
        QVERIFY(prefs.saveError().contains(QStringLiteral("/vela/vela.conf")));
        prefs.reload(); // the compositor reload must not replace the shell error
        QVERIFY(prefs.saveError().contains(confPath()));
        if (shellFirst) {
            QVERIFY(shell.restore());
            prefs.setShowDesktopCorner(false);
            QVERIFY(!prefs.saveError().contains(confPath()));
            QVERIFY(prefs.saveError().contains(QStringLiteral("/vela/vela.conf")));
            QVERIFY(compositor.restore());
            prefs.setScreenOffMinutes(3);
        } else {
            QVERIFY(compositor.restore());
            prefs.setScreenOffMinutes(3);
            QVERIFY(prefs.saveError().contains(confPath()));
            QVERIFY(!prefs.saveError().contains(QStringLiteral("/vela/vela.conf")));
            QVERIFY(shell.restore());
            prefs.setShowDesktopCorner(false);
        }
        QCOMPARE(prefs.saveError(), QString());
    }

    void appModeFailureDoesNotLaunchAppearanceHelpers()
    {
        const QByteArray originalPath = qgetenv("PATH");
        const QString originalIcons = QIcon::themeName();
        const QStringList originalSearch = QIcon::themeSearchPaths();
        const QPalette originalPalette = QGuiApplication::palette();
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", originalPath);
            qunsetenv("VELA_TEST_MODE_PROBE");
            QIcon::setThemeName(originalIcons);
            QIcon::setThemeSearchPaths(originalSearch);
            QGuiApplication::setPalette(originalPalette);
        });
        const QString bin = home().filePath(QStringLiteral("mode-bin"));
        const QString probe = home().filePath(QStringLiteral("mode-probe"));
        QVERIFY(QFile::remove(probe) || !QFileInfo::exists(probe));
        for (const QString& helper : { QStringLiteral("gsettings"), QStringLiteral("plasma-apply-colorscheme"),
                 QStringLiteral("kwriteconfig6") }) {
            const QString relative = QStringLiteral("mode-bin/") + helper;
            QVERIFY(writeFile(relative, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$VELA_TEST_MODE_PROBE\"\n"));
            QVERIFY(QFile::setPermissions(home().filePath(relative),
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        }
        qputenv("PATH", bin.toUtf8());
        qputenv("VELA_TEST_MODE_PROBE", probe.toUtf8());
        // No light/dark variants exist for this private icon theme, so a
        // successful test cannot launch the absolute plasma-changeicons helper.
        QIcon::setThemeName(QStringLiteral("preferences-test-icons"));
        QIcon::setThemeSearchPaths({ home().filePath(QStringLiteral("private-icons")) });
        Preferences prefs;
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        prefs.setAppTheme(QStringLiteral("light"));
        QCOMPARE(prefs.appTheme(), QStringLiteral("dark"));
        QVERIFY(!prefs.saveError().isEmpty());
        QCOMPARE(QIcon::themeName(), QStringLiteral("preferences-test-icons"));
        QCOMPARE(QGuiApplication::palette(), originalPalette);
        QTest::qWait(100);
        QVERIFY(!QFileInfo::exists(probe));
        QVERIFY(blocked.restore());
        prefs.setAppTheme(QStringLiteral("light"));
        QCOMPARE(prefs.appTheme(), QStringLiteral("light"));
        QCOMPARE(prefs.saveError(), QString());
        const auto calls = [&] {
            QFile file(probe);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        QTRY_VERIFY_WITH_TIMEOUT(calls().contains("set org.gnome.desktop.interface color-scheme prefer-light"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(calls().contains("BreezeLight"), 5000);
        expectFreshShell({ { QStringLiteral("appearance/appTheme"), QStringLiteral("light") } });
    }


    void failedWritesRestoreBoundSwitchAndChoice()
    {
        Preferences prefs;
        QQmlEngine engine;
        vela::controls::install(engine, Appearance::Mode::Apps);
        engine.rootContext()->setContextProperty(QStringLiteral("Prefs"), &prefs);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import Vela.Controls
            import "."
            Window {
                width: 480; height: 180; visible: true
                Switch {
                    objectName: "taskViewSwitch"
                    x: 20; y: 20
                    checked: Prefs.taskView
                    onToggled: Prefs.taskView = checked
                }
                Choice {
                    objectName: "alignmentChoice"
                    x: 200; y: 20
                    model: ["Center", "Left"]
                    currentIndex: Prefs.taskbarAlignment === "left" ? 1 : 0
                    onChosen: index => Prefs.taskbarAlignment = index === 1 ? "left" : "center"
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(SETTINGS_QML_DIR "/PreferencesProbe.qml")));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* window = qobject_cast<QQuickWindow*>(object.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* toggle = window->findChild<QQuickItem*>(QStringLiteral("taskViewSwitch"));
        auto* choice = window->findChild<QQuickItem*>(QStringLiteral("alignmentChoice"));
        QVERIFY(toggle && choice);
        const auto click = [window](QQuickItem* item) {
            const QPointF center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
        };
        const auto chooseLeft = [&] {
            click(choice);
            auto* popup = choice->property("popup").value<QObject*>();
            QVERIFY(popup);
            QTRY_VERIFY(popup->property("visible").toBool());
            QTest::keyClick(window, Qt::Key_End);
            QTest::keyClick(window, Qt::Key_Return);
        };
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        click(toggle);
        QVERIFY(prefs.taskView());
        QVERIFY(toggle->property("checked").toBool());
        QVERIFY(!prefs.saveError().isEmpty());
        chooseLeft();
        QCOMPARE(prefs.taskbarAlignment(), QStringLiteral("center"));
        QCOMPARE(choice->property("currentIndex").toInt(), 0);
        QVERIFY(blocked.restore());
        click(toggle);
        QVERIFY(!prefs.taskView());
        QVERIFY(!toggle->property("checked").toBool());
        chooseLeft();
        QCOMPARE(prefs.taskbarAlignment(), QStringLiteral("left"));
        QCOMPARE(choice->property("currentIndex").toInt(), 1);
        QCOMPARE(prefs.saveError(), QString());
    }

    void failedWritesDoNotOverrideALaterExternalWriter_data()
    {
        QTest::addColumn<bool>("present");
        QTest::newRow("key-was-absent") << false;
        QTest::newRow("key-existed") << true;
    }

    void failedWritesDoNotOverrideALaterExternalWriter()
    {
        QFETCH(bool, present);
        if (present) write("[appearance]\nappTheme=dark\n[taskbar]\nalignment=center\n");
        Preferences prefs;
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        prefs.setTaskbarAlignment(QStringLiteral("left"));
        QCOMPARE(prefs.taskbarAlignment(), QStringLiteral("center"));
        QVERIFY(!prefs.saveError().isEmpty());
        QVERIFY(blocked.restore());
        QProcess writer;
        writer.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--write-shell"), confPath(),
            QStringLiteral("taskbar/alignment"), QStringLiteral("left") });
        QVERIFY(writer.waitForFinished(5000));
        QCOMPARE(writer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(writer.exitCode(), 0);
        // Neither a deferred flush nor a later sync may replay the failed
        // update or a queued rollback over this legitimate external choice.
        QCoreApplication::processEvents();
        prefs.reload();
        QCOMPARE(prefs.taskbarAlignment(), QStringLiteral("left"));
        expectFreshShell({ { QStringLiteral("taskbar/alignment"), QStringLiteral("left") } });
    }

    void busyWriterLeavesTheConfigurationUntouched()
    {
        Preferences prefs;
        QLockFile otherWriter(confPath() + QStringLiteral(".lock"));
        QVERIFY(otherWriter.tryLock());
        prefs.setTaskView(false);
        QVERIFY(prefs.taskView());
        QVERIFY(prefs.saveError().contains(confPath()));
        otherWriter.unlock();
        expectFreshShell({}, { QStringLiteral("taskbar/taskView") });
        prefs.setTaskView(false);
        QVERIFY(!prefs.taskView());
        QCOMPARE(prefs.saveError(), QString());
    }

    void malformedShellFileIsNotOverwritten()
    {
        Preferences prefs;
        const QByteArray broken = "[appearance]\nappTheme=dark\n[broken\nvalue=1\n";
        write(broken);
        prefs.setTaskView(false);
        QVERIFY(prefs.taskView());
        QVERIFY(prefs.saveError().contains(confPath()));
        QFile file(confPath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), broken);
    }

    void rapidShellWritesRefreshACachedReaderAfterBothWrites()
    {
        Preferences prefs;
        prefs.setAccent(QColor(QStringLiteral("#112233")));
        QVERIFY(prefs.saveError().isEmpty());
        // A future seed also exercises a stopped/backward wall clock and
        // makes timestamp reuse fail deterministically in the old writer.
        QFile file(confPath());
        QVERIFY(file.open(QIODevice::ReadWrite));
        const QDateTime tick = QDateTime::currentDateTimeUtc().addSecs(60);
        QVERIFY(file.setFileTime(tick, QFileDevice::FileModificationTime));
        file.close();
        QDateTime previous = QFileInfo(confPath()).lastModified();
        QProcess reader;
        reader.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--watch-shell") });
        QVERIFY(reader.waitForReadyRead(5000));
        QCOMPARE(reader.readLine().trimmed(), QByteArray("#112233"));
        for (const QString& color : { QStringLiteral("#445566"), QStringLiteral("#778899") }) {
            prefs.setAccent(QColor(color));
            QVERIFY(prefs.saveError().isEmpty());
            const QDateTime current = QFileInfo(confPath()).lastModified();
            QVERIFY2(current > previous, "a save reused a reader-visible timestamp");
            previous = current;
        }
        reader.write("sync\n");
        QVERIFY(reader.waitForBytesWritten(5000));
        QVERIFY(reader.waitForFinished(5000));
        QCOMPARE(reader.exitStatus(), QProcess::NormalExit);
        QCOMPARE(reader.exitCode(), 0);
        QCOMPARE(reader.readAllStandardOutput().trimmed(), QByteArray("#778899"));
    }

    void malformedShellFileKeepsNonDefaultShownValues()
    {
        Preferences prefs;
        prefs.setTaskView(false);
        QVERIFY(!prefs.taskView());
        const QByteArray broken = "[appearance]\nappTheme=dark\n[broken\nvalue=1\n";
        write(broken);
        prefs.setTaskView(true);
        QVERIFY(!prefs.taskView());
        QVERIFY(prefs.saveError().contains(confPath()));
        QFile file(confPath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), broken);
    }

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
    if (argc == 5 && QByteArray(argv[1]) == "--write-shell") {
        QCoreApplication app(argc, argv);
        QSettings settings(QString::fromLocal8Bit(argv[2]), QSettings::IniFormat);
        settings.setValue(QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4]));
        settings.sync();
        return settings.status() == QSettings::NoError ? 0 : 1;
    }
    if (argc == 2 && QByteArray(argv[1]) == "--watch-shell") {
        QCoreApplication app(argc, argv);
        QSettings settings(QStringLiteral("Vela"), QStringLiteral("vela-shell"));
        settings.setFallbacksEnabled(false);
        QTextStream output(stdout);
        output << settings.value(QStringLiteral("appearance/accent")).toString() << Qt::endl;
        QTextStream input(stdin);
        (void)input.readLine();
        settings.sync();
        output << settings.value(QStringLiteral("appearance/accent")).toString() << Qt::endl;
        return settings.status() == QSettings::NoError ? 0 : 1;
    }
    if (argc == 2 && QByteArray(argv[1]) == "--read-shell") {
        QCoreApplication app(argc, argv);
        QSettings settings(QStringLiteral("Vela"), QStringLiteral("vela-shell"));
        settings.setFallbacksEnabled(false);
        settings.sync();
        QVariantMap values;
        for (const QString& key : settings.allKeys()) values.insert(key, settings.value(key));
        QTextStream(stdout) << QJsonDocument::fromVariant(values).toJson(QJsonDocument::Compact);
        return settings.status() == QSettings::NoError ? 0 : 1;
    }

    // A distribution whose logo is a loose file in pixmaps/.
    if (!isolate()
        || !writeFile(QStringLiteral("os-release"), "NAME=Test\nPRETTY_NAME=\"Test Linux 2\"\nLOGO=test-logo\n")
        || !writeFile(QStringLiteral("data/pixmaps/test-logo.png"), "png")) {
        return 1;
    }

    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-settings"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    PreferencesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "preferences_test.moc"
