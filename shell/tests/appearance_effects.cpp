// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Style.animationEffects and Style.animationDuration (docs/visual-coherence.md
// §6.5): read from animation-effects and animation-duration in vela.conf (on,
// at Vela's durations, by default), and followed live when Settings replaces
// the file. And where the accent comes from (accentsource.h): the theme's,
// the wallpaper's or the chosen one, and the tonal palette that follows it.

#include "appearance.h"
#include "mica.h"
#include "scheme.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSaveFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <iterator>

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

bool writeTheme(const QString& name, const QByteArray& text)
{
    const QString themes = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vela/themes");
    QDir().mkpath(themes);
    QSaveFile file(themes + u'/' + name + QStringLiteral(".conf"));
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size() && file.commit();
}

// Settings' keys in vela-shell.conf: the chosen accent and where it comes
// from; an empty value removes the key.
void setShell(const QString& accent, const QString& source)
{
    QSettings shell(QStringLiteral("Vela"), QStringLiteral("vela-shell"));
    for (const auto& [key, value] : { std::pair { QStringLiteral("appearance/accent"), accent },
             std::pair { QStringLiteral("appearance/accentSource"), source } }) {
        if (value.isEmpty()) {
            shell.remove(key);
        } else {
            shell.setValue(key, value);
        }
    }
    shell.sync();
}

const QColor chosen(QStringLiteral("#8a2be2"));
const QColor wallpaper(QStringLiteral("#2e7d32"));
const QColor themeAccent(QStringLiteral("#c00000"));

} // namespace

class AppearanceEffectsTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        QFile::remove(velaConf());
        QFile::remove(wallpaperColorCachePath());
        setShell(QString(), QString());
    }

    // 1. A theme's accent wins over everything: the wallpaper asked for by
    // the user or the theme, the chosen one. Style.accent stays the user's
    // (Theme.qml puts the theme's over it), the palette starts from the
    // theme's.
    void themeAccentWins()
    {
        QVERIFY(writeTheme(QStringLiteral("painted"), "accent=#c00000\ncolor-source=wallpaper\n"));
        QVERIFY(writeConf("theme=painted\n"));
        saveWallpaperColor(wallpaper);
        setShell(chosen.name(), QStringLiteral("wallpaper"));
        const Appearance appearance;
        QCOMPARE(appearance.accent(), wallpaper);
        const vela::accent::Resolved effective = vela::accent::effectiveAccent(
            appearance.theme(), appearance.light(), vela::accent::Source::Wallpaper, chosen, wallpaper);
        QCOMPARE(effective.color, themeAccent);
        QCOMPARE(effective.origin, vela::accent::Origin::Theme);
        QCOMPARE(appearance.schemeSource(appearance.light()), themeAccent);
        QCOMPARE(appearance.tonal(), appearance.tonalOf(themeAccent, appearance.light()));
        QVERIFY(appearance.tonal() != appearance.tonalOf(wallpaper, appearance.light()));
        // A theme's primary is the palette's source over its accent.
        QVERIFY(writeTheme(QStringLiteral("painted"), "accent=#c00000\nprimary=#1565c0\n"));
        const Appearance withPrimary;
        QCOMPARE(withPrimary.tonal(), withPrimary.tonalOf(QColor(QStringLiteral("#1565c0")), withPrimary.light()));
    }

    // 2. The wallpaper's, when the user asks or leaves it to a theme that
    // asks, and only if the wallpaper has one.
    void wallpaperWhenAsked()
    {
        saveWallpaperColor(wallpaper);
        setShell(chosen.name(), QStringLiteral("wallpaper"));
        QCOMPARE(Appearance().accent(), wallpaper);
        QCOMPARE(Appearance().tonal(), Appearance().tonalOf(wallpaper, Appearance().light()));

        QVERIFY(writeTheme(QStringLiteral("tonal"), "color-source=wallpaper\n"));
        QVERIFY(writeConf("theme=tonal\n"));
        setShell(chosen.name(), QStringLiteral("theme"));
        QCOMPARE(Appearance().accent(), wallpaper);
        setShell(chosen.name(), QString()); // theme is the default
        QCOMPARE(Appearance().accent(), wallpaper);
        const vela::accent::Resolved resolved
            = vela::accent::effectiveAccent(Appearance().theme(), false, vela::accent::Source::Theme, chosen, wallpaper);
        QCOMPARE(resolved.origin, vela::accent::Origin::Wallpaper);

        // A theme that doesn't ask leaves the chosen one.
        QVERIFY(writeTheme(QStringLiteral("tonal"), "color-source=accent\n"));
        QCOMPARE(Appearance().accent(), chosen);

        // A grayscale wallpaper (an empty cache) or none yet: the chosen one.
        QVERIFY(writeTheme(QStringLiteral("tonal"), "color-source=wallpaper\n"));
        saveWallpaperColor(QColor());
        QVERIFY(!cachedWallpaperColor().isValid());
        QCOMPARE(Appearance().accent(), chosen);
        QFile::remove(wallpaperColorCachePath());
        QCOMPARE(Appearance().accent(), chosen);
    }

    // 3. Otherwise the chosen one, also over a theme that asks for the
    // wallpaper; Vela's own when none was chosen.
    void chosenOtherwise()
    {
        saveWallpaperColor(wallpaper);
        QVERIFY(writeTheme(QStringLiteral("tonal"), "color-source=wallpaper\n"));
        QVERIFY(writeConf("theme=tonal\n"));
        setShell(chosen.name(), QStringLiteral("chosen"));
        const Appearance appearance;
        QCOMPARE(appearance.accent(), chosen);
        QCOMPARE(appearance.tonal(), appearance.tonalOf(chosen, appearance.light()));
        QVERIFY(writeConf("theme=\n"));
        setShell(QString(), QString());
        const Appearance fresh;
        QCOMPARE(fresh.accent(), QColor(0x5b, 0x8c, 0xff));
        QCOMPARE(fresh.tonal().value(QStringLiteral("primary")).value<QColor>(), QColor(QStringLiteral("#b2c5ff"))); // dark
        QCOMPARE(fresh.tonal().size(), int(std::size(vela::color::roleNames)));
    }

    // The palette follows the wallpaper live: the shell rewrites the cache.
    void followsTheWallpaperColor()
    {
        saveWallpaperColor(wallpaper);
        setShell(chosen.name(), QStringLiteral("wallpaper"));
        Appearance appearance;
        appearance.watch();
        QSignalSpy changed(&appearance, &Appearance::changed);
        const QColor next(QStringLiteral("#ef6c00"));
        saveWallpaperColor(next);
        QTRY_COMPARE(appearance.accent(), next);
        QCOMPARE(appearance.tonal(), appearance.tonalOf(next, appearance.light()));
        QVERIFY(changed.count() >= 1);
    }

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
