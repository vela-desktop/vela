// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The icon provider's prefixes (src/iconprovider.h): "l/" and "d/" leave the
// drawing as it is, "w/" makes it white, "t<rrggbb>/" that color (a tile's
// content on a tonal fill), and what only looks like a prefix is a name.

#include "iconprovider.h"

#include <QGuiApplication>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

class IconProviderTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_icon;

    QColor drawn(const QString& prefix)
    {
        IconProvider provider;
        QSize size;
        const QImage image = provider.requestPixmap(prefix + m_icon, &size, QSize(16, 16)).toImage();
        return image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
    }

    // kdeglobals of the test's own: the host's [Icons] Theme would choose
    // the theme.
    QString kdeglobals() const { return m_dir.filePath(QStringLiteral("config/kdeglobals")); }

    void writeTheme(const QString& name)
    {
        const QString theme = m_dir.filePath(QStringLiteral("data/icons/") + name);
        QVERIFY(QDir().mkpath(theme + QStringLiteral("/16x16/actions")));
        QFile index(theme + QStringLiteral("/index.theme"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("[Icon Theme]\nName=Vela test icons\nDirectories=16x16/actions\n"
                    "[16x16/actions]\nSize=16\nType=Fixed\nContext=Actions\n");
        index.close();
        const QString icon = theme + QStringLiteral("/16x16/actions/vela-test-action.png");
        QVERIFY(QFile::exists(icon) || QFile::copy(m_icon, icon));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath(QStringLiteral("config")).toUtf8());
        m_icon = m_dir.filePath(QStringLiteral("drawing.png"));
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(QColor(200, 30, 30));
        QVERIFY(image.save(m_icon));
    }

    void prefixes()
    {
        const QColor own(200, 30, 30);
        QCOMPARE(drawn(QString()), own);
        QCOMPARE(drawn(QStringLiteral("l/")), own);
        QCOMPARE(drawn(QStringLiteral("d/")), own);
        QCOMPARE(drawn(QStringLiteral("w/")), QColor(Qt::white));
        QCOMPARE(drawn(QStringLiteral("t102030/")), QColor(0x10, 0x20, 0x30));
        QCOMPARE(drawn(QStringLiteral("tffffff/")), QColor(Qt::white));
    }

    // Not a color: the whole address is a name, which no theme has, so the
    // fallback drawing (or nothing) comes, never a tinted one.
    void notAColor()
    {
        QVERIFY(drawn(QStringLiteral("tzzzzzz/")) != QColor(0x10, 0x20, 0x30));
    }

    void xdgThemesWithoutAPlatformPlugin()
    {
        const auto originalPaths = QIcon::themeSearchPaths();
        const auto originalTheme = QIcon::themeName();
        const auto originalData = qgetenv("XDG_DATA_HOME");
        const auto restore = qScopeGuard([&] {
            QIcon::setThemeSearchPaths(originalPaths);
            QIcon::setThemeName(originalTheme);
            if (originalData.isNull()) qunsetenv("XDG_DATA_HOME");
            else qputenv("XDG_DATA_HOME", originalData);
        });
        const QString data = m_dir.filePath(QStringLiteral("data"));
        for (const QString& name : { QStringLiteral("vela-test-icons"), QStringLiteral("vela-test-icons-dark") }) {
            writeTheme(name);
        }
        QFile::remove(kdeglobals());
        qputenv("XDG_DATA_HOME", data.toUtf8());
        QIcon::setThemeSearchPaths({ QStringLiteral(":/icons") });
        QIcon::setThemeName(QStringLiteral("vela-test-icons"));
        QVERIFY(QIcon::fromTheme(QStringLiteral("vela-test-action")).isNull());
        for (bool light : { false, true }) {
            applyIconTheme(light);
            QCOMPARE(QIcon::themeName(), light ? QStringLiteral("vela-test-icons") : QStringLiteral("vela-test-icons-dark"));
            QVERIFY(QIcon::themeSearchPaths().contains(QStringLiteral(":/icons")));
            IconProvider provider;
            const auto pixmap = provider.requestPixmap(QStringLiteral("vela-test-action"), nullptr, QSize(16, 16));
            QVERIFY(!pixmap.isNull());
            QCOMPARE(pixmap.toImage().pixelColor(8, 8), QColor(200, 30, 30));
        }
    }

    // The theme chosen in Settings ([Icons] Theme) wins over the platform's,
    // and is read again at every call: no restart.
    void configuredThemeIsReadAgain()
    {
        const auto originalTheme = QIcon::themeName();
        const auto originalData = qgetenv("XDG_DATA_HOME");
        const auto restore = qScopeGuard([&] {
            QIcon::setThemeName(originalTheme);
            QFile::remove(kdeglobals());
            if (originalData.isNull())
                qunsetenv("XDG_DATA_HOME");
            else
                qputenv("XDG_DATA_HOME", originalData);
        });
        for (const QString& name : { QStringLiteral("vela-chosen"), QStringLiteral("vela-chosen-dark"),
                 QStringLiteral("vela-other"), QStringLiteral("vela-other-dark") }) {
            writeTheme(name);
        }
        qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
        QIcon::setThemeName(QStringLiteral("vela-other"));
        QVERIFY(vela::kde::write(
            kdeglobals(), { { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("vela-chosen") } }));
        applyIconTheme(false);
        QCOMPARE(QIcon::themeName(), QStringLiteral("vela-chosen-dark"));
        applyIconTheme(true);
        QCOMPARE(QIcon::themeName(), QStringLiteral("vela-chosen"));
        QVERIFY(vela::kde::write(
            kdeglobals(), { { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("vela-other") } }));
        applyIconTheme(true);
        QCOMPARE(QIcon::themeName(), QStringLiteral("vela-other"));
        // The variant is Vela's own: the choice stays as it was written.
        applyIconTheme(false);
        QCOMPARE(QIcon::themeName(), QStringLiteral("vela-other-dark"));
        QCOMPARE(vela::kde::read(kdeglobals(), QStringLiteral("Icons"), QStringLiteral("Theme")),
            QStringLiteral("vela-other"));
    }

    // hicolor chosen in Settings stays hicolor; hicolor only because the
    // platform has nothing better falls back to VELA_ICON_THEME.
    void hicolorChosenOrNot()
    {
        const auto originalTheme = QIcon::themeName();
        const auto restore = qScopeGuard([&] {
            QIcon::setThemeName(originalTheme);
            QFile::remove(kdeglobals());
            qunsetenv("VELA_ICON_THEME");
        });
        qputenv("VELA_ICON_THEME", "vela-fallback");
        QIcon::setThemeName(QStringLiteral("hicolor"));
        QFile::remove(kdeglobals());
        QCOMPARE(configuredIconTheme(), QStringLiteral("vela-fallback"));
        QVERIFY(vela::kde::write(kdeglobals(), { { QStringLiteral("Icons"), QStringLiteral("Theme"), QString() } }));
        QCOMPARE(configuredIconTheme(), QStringLiteral("vela-fallback"));
        QVERIFY(vela::kde::write(
            kdeglobals(), { { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("hicolor") } }));
        QCOMPARE(configuredIconTheme(), QStringLiteral("hicolor"));
        applyIconTheme(false);
        QCOMPARE(QIcon::themeName(), QStringLiteral("hicolor"));
    }

    // Settings offers the themes in ~/.icons too: QIcon must find them.
    void legacyHomeIcons()
    {
        const auto originalPaths = QIcon::themeSearchPaths();
        const auto restore = qScopeGuard([&] { QIcon::setThemeSearchPaths(originalPaths); });
        QIcon::setThemeSearchPaths({ QStringLiteral(":/icons") });
        applyIconTheme(false);
        applyIconTheme(true);
        const QStringList paths = QIcon::themeSearchPaths();
        QCOMPARE(paths.count(QDir::homePath() + QStringLiteral("/.icons")), 1);
        QCOMPARE(paths.constFirst(), QStringLiteral(":/icons"));
    }
};

QTEST_MAIN(IconProviderTest)
#include "iconprovider.moc"
