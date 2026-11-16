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

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
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
            const QString theme = data + QStringLiteral("/icons/") + name;
            QVERIFY(QDir().mkpath(theme + QStringLiteral("/16x16/actions")));
            QFile index(theme + QStringLiteral("/index.theme"));
            QVERIFY(index.open(QIODevice::WriteOnly));
            index.write("[Icon Theme]\nName=Vela test icons\nDirectories=16x16/actions\n"
                        "[16x16/actions]\nSize=16\nType=Fixed\nContext=Actions\n");
            index.close();
            QVERIFY(QFile::copy(m_icon, theme + QStringLiteral("/16x16/actions/vela-test-action.png")));
        }
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
};

QTEST_MAIN(IconProviderTest)
#include "iconprovider.moc"
