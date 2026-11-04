// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests of theme files (themefile.h): what a line becomes, which file a name
// picks (the user's over the installed ones), the layers of a theme and its
// styles, and that the files in themes/ only use tokens Theme.qml, Styles.qml
// and the shell read, each style its own surface's. All in temporary
// directories (XDG_CONFIG_HOME, XDG_DATA_DIRS).
//
//   ctest --test-dir build -R themefile      (or build/shell/vela-themefile-test)

#include "themefile.h"

#include <QBuffer>
#include <QUrl>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace {

void write(const QString& path, const QByteArray& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(text);
}

QVariantMap parse(const QByteArray& text)
{
    QBuffer buffer;
    buffer.setData(text);
    buffer.open(QIODevice::ReadOnly | QIODevice::Text);
    return vela::themes::parse(buffer, QStringLiteral("test"));
}

} // namespace

class ThemeFileTest : public QObject {
    Q_OBJECT

    static QString config() { return qEnvironmentVariable("XDG_CONFIG_HOME"); }
    static QString data() { return qEnvironmentVariable("XDG_DATA_DIRS").section(u':', 0, 0); }

private slots:
    void init()
    {
        QDir(config()).removeRecursively();
        QDir(data()).removeRecursively();
    }

    void colorsAndNumbers()
    {
        const QVariantMap map = parse("# a comment\n\n"
                                      "dark.text=#E0DEF4\n"
                                      "light.taskbar=#E6FAF4ED\n"
                                      " radius = 6 \n"
                                      "accent=#c4a7e7\n");
        QCOMPARE(map.size(), 4);
        QCOMPARE(map.value(QStringLiteral("dark.text")).value<QColor>(), QColor(0xE0, 0xDE, 0xF4));
        const QColor taskbar = map.value(QStringLiteral("light.taskbar")).value<QColor>();
        QCOMPARE(taskbar.alpha(), 0xE6); // #AARRGGBB
        QCOMPARE(taskbar.rgb() & 0xFFFFFF, 0xFAF4EDu);
        QCOMPARE(map.value(QStringLiteral("radius")).typeId(), QMetaType::Double);
        QCOMPARE(map.value(QStringLiteral("radius")).toDouble(), 6.0);
    }

    // Not a color, a number nor a picture: skipped, with a warning that says where.
    void badLinesAreSkipped()
    {
        QTest::ignoreMessage(QtWarningMsg, "theme test:1: accent is neither a color, a number nor an asset:");
        QTest::ignoreMessage(QtWarningMsg, "theme test:2: text is neither a color, a number nor an asset:");
        QTest::ignoreMessage(QtWarningMsg, "theme test:3: not a key=value line");
        QTest::ignoreMessage(QtWarningMsg, "theme test:4: not a key=value line");
        const QVariantMap map = parse("accent=red\ntext=#12345\nradius\n=4\ncard=#ffffff\n");
        QCOMPARE(map.keys(), QStringList { QStringLiteral("card") });
    }

    // The same lookup as Theme.qml's themed(): the mode's key, then the shared one.
    void themedLookup()
    {
        const QVariantMap map = parse("accent=#111111\ndark.accent=#222222\nlight.background=#333333\n");
        QCOMPARE(vela::themes::themed(map, QStringLiteral("accent"), false).value<QColor>(), QColor(0x22, 0x22, 0x22));
        QCOMPARE(vela::themes::themed(map, QStringLiteral("accent"), true).value<QColor>(), QColor(0x11, 0x11, 0x11));
        QCOMPARE(vela::themes::themed(map, QStringLiteral("background"), true).value<QColor>(), QColor(0x33, 0x33, 0x33));
        QVERIFY(!vela::themes::themed(map, QStringLiteral("background"), false).isValid());
    }

    // asset:<file>: the installed picture's URL, "" if missing; never outside.
    void assets()
    {
        write(data() + QStringLiteral("/vela/themes/assets/aero/orb.png"), "png");
        write(data() + QStringLiteral("/vela/secret.png"), "png");
        const QVariantMap map = parse("taskbar-orb=asset:aero/orb.png\ntaskbar-tile-image=asset:aero/missing.png\n"
                                      "taskbar-reflection=asset:../../secret.png\n");
        QCOMPARE(map.value(QStringLiteral("taskbar-orb")).toString(),
            QUrl::fromLocalFile(data() + QStringLiteral("/vela/themes/assets/aero/orb.png")).toString());
        QCOMPARE(map.value(QStringLiteral("taskbar-tile-image")).toString(), QString());
        QCOMPARE(map.value(QStringLiteral("taskbar-reflection")).toString(), QString());
    }

    // theme= and the three styles: each layer on its own, a style over the
    // theme, a panel's look from the theme and its own style only.
    void layers()
    {
        write(data() + QStringLiteral("/vela/themes/glass.conf"), "blur-strength=0.5\nradius=6\n");
        write(data() + QStringLiteral("/vela/themes/taskbar/aero.conf"), "blur-strength=2\ntaskbar-height=40\n");
        write(data() + QStringLiteral("/vela/themes/startmenu/big.conf"), "startmenu-width=900\n");
        const QString conf = config() + QStringLiteral("/vela/vela.conf");
        write(conf, "theme=glass\ntaskbar-style=aero\nstartmenu-style=big\nnotifications-style=missing\n");
        const vela::themes::Loaded loaded = vela::themes::loadConfigured(conf);
        QVERIFY(!loaded.files[vela::themes::Taskbar].isEmpty());
        QVERIFY(loaded.files[vela::themes::Notifications].isEmpty()); // not installed
        const QVariantMap merged = loaded.merged();
        QCOMPARE(merged.value(QStringLiteral("radius")).toDouble(), 6.0);
        QCOMPARE(merged.value(QStringLiteral("taskbar-height")).toDouble(), 40.0);
        QCOMPARE(merged.value(QStringLiteral("startmenu-width")).toDouble(), 900.0);
        QCOMPARE(loaded.withStyle(vela::themes::Taskbar).value(QStringLiteral("blur-strength")).toDouble(), 2.0);
        // The Start menu's blur doesn't take the taskbar style's.
        QCOMPARE(loaded.withStyle(vela::themes::StartMenu).value(QStringLiteral("blur-strength")).toDouble(), 0.5);
        QCOMPARE(loaded.withStyle(vela::themes::Theme).value(QStringLiteral("blur-strength")).toDouble(), 0.5);
    }

    // Every style, with its pictures if it has any.
    void gallery()
    {
        write(data() + QStringLiteral("/vela/themes/taskbar/plain.conf"), "taskbar-height=40\n");
        write(data() + QStringLiteral("/vela/themes/taskbar/shown.conf"), "# Shown: one\ntaskbar-height=40\n");
        write(data() + QStringLiteral("/vela/themes/screenshots/taskbar/shown.png"), "png");
        write(data() + QStringLiteral("/vela/themes/notifications/glass.conf"), "notifications-radius=8\n");
        write(data() + QStringLiteral("/vela/themes/screenshots/notifications/glass-qs.png"), "png");
        write(data() + QStringLiteral("/vela/themes/screenshots/notifications/glass-nc.png"), "png");
        const QVariantList taskbar = vela::themes::gallery(QStringLiteral("taskbar"));
        QCOMPARE(taskbar.size(), 2);
        QCOMPARE(taskbar[0].toMap().value(QStringLiteral("images")).toStringList().size(), 0);
        QCOMPARE(taskbar[1].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Shown"));
        QCOMPARE(taskbar[1].toMap().value(QStringLiteral("images")).toStringList().size(), 1);
        const QStringList glass = vela::themes::gallery(QStringLiteral("notifications"))[0].toMap().value(QStringLiteral("images")).toStringList();
        QCOMPARE(glass.size(), 2);
        QVERIFY(glass[0].endsWith(QLatin1String("glass-qs.png"))); // quick settings, then the center
        QVERIFY(vela::themes::gallery(QString()).isEmpty()); // themes have no gallery
        QVERIFY(vela::themes::gallery(QStringLiteral("../x")).isEmpty());
    }

    void configuredNameIsTheLastOne()
    {
        const QString conf = config() + QStringLiteral("/vela/vela.conf");
        QCOMPARE(vela::themes::configuredName(conf), QString()); // no file
        write(conf, "language=it\ntheme=everblush\nscreen-off=5\ntheme= rose-pine \n");
        QCOMPARE(vela::themes::configuredName(conf), QStringLiteral("rose-pine"));
    }

    void namesStayInTheThemesDirectory()
    {
        write(config() + QStringLiteral("/vela/secret.conf"), "text=#ff0000\n");
        for (const char* name : { "", "../secret", "a/b", ".hidden", "/etc/passwd" }) {
            QVERIFY2(!vela::themes::validName(QLatin1String(name)), name);
            QCOMPARE(vela::themes::locate(QLatin1String(name)), QString());
        }
    }

    void theUsersThemeWins()
    {
        write(data() + QStringLiteral("/vela/themes/mine.conf"), "# Installed: one\n");
        write(data() + QStringLiteral("/vela/themes/other.conf"), "no title line\n");
        QCOMPARE(vela::themes::locate(QStringLiteral("mine")), data() + QStringLiteral("/vela/themes/mine.conf"));
        write(config() + QStringLiteral("/vela/themes/mine.conf"), "# Mine: the user's copy\n");
        QCOMPARE(vela::themes::locate(QStringLiteral("mine")), config() + QStringLiteral("/vela/themes/mine.conf"));
        QCOMPARE(vela::themes::locate(QStringLiteral("missing")), QString());

        const QVariantList list = vela::themes::available();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("mine"));
        QCOMPARE(list[0].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Mine")); // not "Installed"
        QCOMPARE(list[1].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("other"));
    }

    // Every key of themes/ is a token read by Theme.qml (themed("...")),
    // Styles.qml (size(), flag(), picture()) or the shell (the blur's,
    // main.cpp), and every line parses: a typo fails here instead of doing
    // nothing. A style sets only its own surface's keys (and its blur), so
    // styles of different surfaces never change each other.
    void shippedThemesUseKnownTokens()
    {
        QSet<QString> tokens;
        const QRegularExpression read(QStringLiteral("(?:themed|size|flag|picture)\\(\"([^\"]+)\"|\"(blur-[a-z]+)\""));
        for (const char* source : { THEME_QML, STYLES_QML, SHELL_MAIN }) {
            QFile file(QString::fromUtf8(source));
            QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), source);
            for (auto it = read.globalMatch(QString::fromUtf8(file.readAll())); it.hasNext();) {
                const QRegularExpressionMatch match = it.next();
                tokens.insert(match.captured(1).isEmpty() ? match.captured(2) : match.captured(1));
            }
        }
        for (const char* token : { "accent", "taskbar-height", "startmenu-layout", "quicksettings-radius", "menu-layout", "blur-tint" }) {
            QVERIFY2(tokens.contains(QLatin1String(token)), token);
        }

        // The prefixes each kind of file may set, besides the blur's.
        const QList<std::pair<QString, QStringList>> kinds {
            { QString(), {} }, // a theme: any token
            { QStringLiteral("taskbar"), { QStringLiteral("taskbar") } },
            { QStringLiteral("startmenu"), { QStringLiteral("startmenu-"), QStringLiteral("menu-layout") } },
            { QStringLiteral("notifications"), { QStringLiteral("notifications-"), QStringLiteral("quicksettings-") } },
        };
        QTest::failOnWarning(QRegularExpression(QStringLiteral("^theme ")));
        int count = 0;
        for (const auto& [surface, prefixes] : kinds) {
            const QDir dir(QStringLiteral(THEMES_DIR) + (surface.isEmpty() ? QString() : u'/' + surface));
            for (const QString& name : dir.entryList({ QStringLiteral("*.conf") }, QDir::Files)) {
                ++count;
                const QString where = surface.isEmpty() ? name : surface + u'/' + name;
                const QVariantMap map = vela::themes::load(dir.filePath(name));
                for (const QString& key : map.keys()) {
                    QString token = key;
                    if (token.startsWith(QLatin1String("dark.")) || token.startsWith(QLatin1String("light."))) {
                        token = token.section(u'.', 1);
                    }
                    QVERIFY2(tokens.contains(token), qPrintable(where + u':' + key + QStringLiteral(" is read by nothing")));
                    const bool own = prefixes.isEmpty() || token.startsWith(QLatin1String("blur-"))
                        || std::any_of(prefixes.cbegin(), prefixes.cend(), [&token](const QString& p) { return token.startsWith(p); });
                    QVERIFY2(own, qPrintable(where + u':' + key + QStringLiteral(" belongs to another surface")));
                }
            }
        }
        QVERIFY(count >= 84); // the four themes and the styles of lowell's fork
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(dir.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(dir.filePath(QStringLiteral("sys-config"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(dir.filePath(QStringLiteral("data"))));
    qputenv("XDG_DATA_DIRS", QFile::encodeName(dir.filePath(QStringLiteral("sys-data"))));
    QCoreApplication app(argc, argv);
    ThemeFileTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "themefile.moc"
