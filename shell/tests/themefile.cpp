// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests of theme files (themefile.h): what a line becomes, which file a name
// picks (the user's over the installed ones), and that the themes in themes/
// only use tokens Theme.qml reads. All in temporary directories
// (XDG_CONFIG_HOME, XDG_DATA_DIRS).
//
//   ctest --test-dir build -R themefile      (or build/shell/vela-themefile-test)

#include "themefile.h"

#include <QBuffer>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

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

    // Not a color nor a number: skipped, with a warning that says where.
    void badLinesAreSkipped()
    {
        QTest::ignoreMessage(QtWarningMsg, "theme test:1: accent is neither a color nor a number");
        QTest::ignoreMessage(QtWarningMsg, "theme test:2: text is neither a color nor a number");
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

    // Every key of themes/*.conf is a token Theme.qml reads (themed("...")),
    // and every line parses: a typo fails here instead of doing nothing.
    void shippedThemesUseKnownTokens()
    {
        QFile qml(QStringLiteral(THEME_QML));
        QVERIFY(qml.open(QIODevice::ReadOnly | QIODevice::Text));
        QSet<QString> tokens;
        const QRegularExpression themed(QStringLiteral("themed\\(\"([^\"]+)\""));
        for (auto it = themed.globalMatch(QString::fromUtf8(qml.readAll())); it.hasNext();) {
            tokens.insert(it.next().captured(1));
        }
        QVERIFY(tokens.contains(QStringLiteral("accent")));

        const QDir themes(QStringLiteral(THEMES_DIR));
        const QStringList files = themes.entryList({ QStringLiteral("*.conf") }, QDir::Files);
        QVERIFY(!files.isEmpty());
        QTest::failOnWarning(QRegularExpression(QStringLiteral("^theme ")));
        for (const QString& name : files) {
            const QVariantMap map = vela::themes::load(themes.filePath(name));
            QVERIFY2(!map.isEmpty(), qPrintable(name));
            for (const QString& key : map.keys()) {
                QString token = key;
                if (token.startsWith(QLatin1String("dark.")) || token.startsWith(QLatin1String("light."))) {
                    token = token.section(u'.', 1);
                }
                QVERIFY2(tokens.contains(token), qPrintable(name + u':' + key + QStringLiteral(" is not a Theme.qml token")));
            }
        }
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
