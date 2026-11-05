// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The Start button's icon (src/starticon.h): os-release's name and LOGO=,
// found in the icon theme or as a loose file (CachyOS's
// /usr/share/icons/cachyos.svg), and a choice that can't be shown falls
// back to Vela's sail, as the taskbar and Settings both read it.

#include "starticon.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

class StartIconTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    void write(const QString& path, const QByteArray& text)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text);
    }
    QSettings settings(const QByteArray& text)
    {
        const QString path = m_dir.filePath(QStringLiteral("vela-shell.conf"));
        QFile::remove(path);
        write(path, text);
        return QSettings(path, QSettings::IniFormat);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // Only this directory's data: no icon theme, no system logos.
        qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
        qputenv("XDG_DATA_DIRS", m_dir.filePath(QStringLiteral("system")).toUtf8());
        qputenv("VELA_OS_RELEASE", m_dir.filePath(QStringLiteral("os-release")).toUtf8());
        QIcon::setThemeSearchPaths({});
    }

    void readsNameAndLooseLogo()
    {
        write(m_dir.filePath(QStringLiteral("os-release")),
            "NAME=\"Test Linux\"\nPRETTY_NAME='Test Linux 1.0'\nID=test\nLOGO=test-logo\n");
        const QString logo = m_dir.filePath(QStringLiteral("system/icons/test-logo.svg"));
        write(logo, "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
        const auto distribution = vela::start::distribution();
        QCOMPARE(distribution.name, QStringLiteral("Test Linux 1.0"));
        QCOMPARE(distribution.logo, logo);
        // Or in pixmaps/, PNG.
        QFile::remove(logo);
        const QString pixmap = m_dir.filePath(QStringLiteral("data/pixmaps/test-logo.png"));
        write(pixmap, "png");
        QCOMPARE(vela::start::distribution().logo, pixmap);
        QFile::remove(pixmap);
    }

    void noLogoNoName()
    {
        write(m_dir.filePath(QStringLiteral("os-release")), "NAME=Plain\nLOGO=../escape\n");
        QCOMPARE(vela::start::distribution().name, QStringLiteral("Plain"));
        QCOMPARE(vela::start::distribution().logo, QString()); // not a path
        write(m_dir.filePath(QStringLiteral("os-release")), "NAME=Plain\nLOGO=nowhere\n");
        QCOMPARE(vela::start::distribution().logo, QString());
    }

    void readsEveryWayOfQuoting()
    {
        write(m_dir.filePath(QStringLiteral("os-release")),
            "# a comment\nNAME=Plain\nPRETTY_NAME=\"Say \\\"hi\\\"\"\nLOGO=\"x\n");
        QCOMPARE(vela::start::distribution().name, QStringLiteral("Say \"hi\""));
        QCOMPARE(vela::start::distribution().logo, QString()); // "x: no such icon
        // Without PRETTY_NAME, NAME; an empty value.
        write(m_dir.filePath(QStringLiteral("os-release")), "NAME=Plain\nPRETTY_NAME=\nLOGO=\n");
        QCOMPARE(vela::start::distribution().name, QStringLiteral("Plain"));
        QCOMPARE(vela::start::distribution().logo, QString());
    }

    void noOsRelease()
    {
        qputenv("VELA_OS_RELEASE", m_dir.filePath(QStringLiteral("missing")).toUtf8());
        const auto distribution = vela::start::distribution();
        qputenv("VELA_OS_RELEASE", m_dir.filePath(QStringLiteral("os-release")).toUtf8());
        QCOMPARE(distribution.name, QString());
        QCOMPARE(distribution.logo, QString());
    }

    void theSystemsOsRelease()
    {
        // Without the tests' file: /etc/os-release, else /usr/lib's, read as
        // they are (every Linux the CI runs on has one).
        qunsetenv("VELA_OS_RELEASE");
        const auto distribution = vela::start::distribution();
        qputenv("VELA_OS_RELEASE", m_dir.filePath(QStringLiteral("os-release")).toUtf8());
        if (QFileInfo::exists(QStringLiteral("/etc/os-release")) || QFileInfo::exists(QStringLiteral("/usr/lib/os-release"))) {
            QVERIFY(!distribution.name.isEmpty());
        }
    }

    void logoFromTheIconTheme()
    {
        // No loose file: the theme's icon, by its name.
        const QString themes = m_dir.filePath(QStringLiteral("themes"));
        // A PNG: whether Qt reads SVG icons depends on its plugins.
        write(themes + QStringLiteral("/test/index.theme"),
            "[Icon Theme]\nName=Test\nDirectories=apps\n\n[apps]\nSize=48\nType=Fixed\n");
        QVERIFY(QDir().mkpath(themes + QStringLiteral("/test/apps")));
        QImage logo(48, 48, QImage::Format_ARGB32);
        logo.fill(Qt::green);
        QVERIFY(logo.save(themes + QStringLiteral("/test/apps/themed-logo.png")));
        QIcon::setThemeSearchPaths({ themes });
        QIcon::setThemeName(QStringLiteral("test"));
        write(m_dir.filePath(QStringLiteral("os-release")), "NAME=Themed\nLOGO=themed-logo\n");
        QCOMPARE(vela::start::distribution().logo, QStringLiteral("themed-logo"));
        QIcon::setThemeSearchPaths({});
        // An old XPM, loose.
        const QString xpm = m_dir.filePath(QStringLiteral("data/pixmaps/old-logo.xpm"));
        write(xpm, "/* XPM */");
        write(m_dir.filePath(QStringLiteral("os-release")), "NAME=Old\nLOGO=old-logo\n");
        QCOMPARE(vela::start::distribution().logo, xpm);
        QFile::remove(xpm);
    }

    void iconFallsBackToTheSail()
    {
        write(m_dir.filePath(QStringLiteral("os-release")), "NAME=Plain\n");
        QCOMPARE(vela::start::icon(settings("")).kind, QStringLiteral("vela"));
        // The distribution, without a logo.
        auto icon = vela::start::icon(settings("[start]\nicon=distribution\n"));
        QCOMPARE(icon.kind, QStringLiteral("vela"));
        QCOMPARE(icon.source, QString());
        // A picture that is gone.
        icon = vela::start::icon(settings("[start]\nicon=picture\niconPicture=/nowhere/sail.png\n"));
        QCOMPARE(icon.kind, QStringLiteral("vela"));
        // A picture by a relative path: not one the shell can find.
        write(m_dir.filePath(QStringLiteral("relative.png")), "png");
        QCOMPARE(vela::start::icon(settings("[start]\nicon=picture\niconPicture=relative.png\n")).kind, QStringLiteral("vela"));
        // Something unknown.
        QCOMPARE(vela::start::icon(settings("[start]\nicon=orb\n")).kind, QStringLiteral("vela"));
    }

    void iconShowsWhatIsThere()
    {
        write(m_dir.filePath(QStringLiteral("os-release")), "NAME=Test\nLOGO=test-logo\n");
        const QString logo = m_dir.filePath(QStringLiteral("data/icons/test-logo.svg"));
        write(logo, "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
        auto icon = vela::start::icon(settings("[start]\nicon=distribution\n"));
        QCOMPARE(icon.kind, QStringLiteral("distribution"));
        QCOMPARE(icon.source, logo);
        const QString picture = m_dir.filePath(QStringLiteral("sail.png"));
        write(picture, "png");
        icon = vela::start::icon(settings(QByteArray("[start]\nicon=picture\niconPicture=") + picture.toUtf8() + "\n"));
        QCOMPARE(icon.kind, QStringLiteral("picture"));
        QCOMPARE(icon.source, picture);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv); // QIcon's theme lookups
    StartIconTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "starticon.moc"
