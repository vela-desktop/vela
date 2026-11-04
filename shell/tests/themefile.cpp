// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests of theme files (themefile.h): what a line becomes and what is wrong
// with it, which file a name picks (the user's over the installed ones), how
// inherits= chains resolve, the token list generated from Theme.qml, and the
// themes in themes/. All in temporary directories (XDG_CONFIG_HOME,
// XDG_DATA_DIRS).
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

vela::themes::File read(const QByteArray& text)
{
    QBuffer buffer;
    buffer.setData(text);
    buffer.open(QIODevice::ReadOnly | QIODevice::Text);
    return vela::themes::read(buffer, QStringLiteral("test"));
}

QStringList kinds(const QList<vela::themes::Problem>& problems)
{
    QStringList list;
    for (const vela::themes::Problem& problem : problems) {
        list.append(vela::themes::kindName(problem.kind));
    }
    return list;
}

} // namespace

class ThemeFileTest : public QObject {
    Q_OBJECT

    static QString config() { return qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vela/themes/"); }
    static QString data() { return qEnvironmentVariable("XDG_DATA_DIRS").section(u':', 0, 0) + QStringLiteral("/vela/themes/"); }

private slots:
    void init()
    {
        QDir(qEnvironmentVariable("XDG_CONFIG_HOME")).removeRecursively();
        QDir(qEnvironmentVariable("XDG_DATA_DIRS").section(u':', 0, 0)).removeRecursively();
    }

    // The list comes from Theme.qml: every themed("...") token, typed as its
    // property, and nothing else.
    void tokensAreTheOnesOfThemeQml()
    {
        QFile qml(QStringLiteral(THEME_QML));
        QVERIFY(qml.open(QIODevice::ReadOnly | QIODevice::Text));
        QSet<QString> inQml;
        const QRegularExpression themed(QStringLiteral("themed\\(\"([^\"]+)\""));
        for (auto it = themed.globalMatch(QString::fromUtf8(qml.readAll())); it.hasNext();) {
            inQml.insert(it.next().captured(1));
        }
        QSet<QString> listed;
        for (const vela::themes::Token& token : vela::themes::tokens) {
            listed.insert(QLatin1String(token.name));
        }
        QCOMPARE(listed, inQml);
        QCOMPARE(vela::themes::token(QStringLiteral("accent"))->kind, vela::themes::TokenKind::Color);
        QCOMPARE(vela::themes::token(QStringLiteral("radius"))->kind, vela::themes::TokenKind::Number);
        QVERIFY(!vela::themes::token(QStringLiteral("subtleHover"))); // not themed (Theme.qml says why)
    }

    void colorsAndNumbers()
    {
        const vela::themes::File file = read("# a comment\n\n"
                                             "dark.text=#E0DEF4\n"
                                             "light.taskbar=#E6FAF4ED\n"
                                             " radius = 6 \n"
                                             "accent=#c4a7e7\n");
        QVERIFY(file.problems.isEmpty());
        const QVariantMap& map = file.tokens;
        QCOMPARE(map.size(), 4);
        QCOMPARE(map.value(QStringLiteral("dark.text")).value<QColor>(), QColor(0xE0, 0xDE, 0xF4));
        const QColor taskbar = map.value(QStringLiteral("light.taskbar")).value<QColor>();
        QCOMPARE(taskbar.alpha(), 0xE6); // #AARRGGBB
        QCOMPARE(taskbar.rgb() & 0xFFFFFF, 0xFAF4EDu);
        QCOMPARE(map.value(QStringLiteral("radius")).typeId(), QMetaType::Double);
        QCOMPARE(map.value(QStringLiteral("radius")).toDouble(), 6.0);
    }

    // Each wrong line is skipped with a problem that says where and what, in
    // the log too; the good lines stay.
    void wrongLinesSayWhatIsWrong()
    {
        QTest::ignoreMessage(QtWarningMsg, "theme test:1: accent is a color, #RRGGBB or #AARRGGBB, not \"red\": skipped");
        QTest::ignoreMessage(QtWarningMsg, "theme test:2: text is a color, #RRGGBB or #AARRGGBB, not \"#12345\": skipped");
        QTest::ignoreMessage(QtWarningMsg, "theme test:3: radius is a number, not \"#FF00FF\": skipped");
        QTest::ignoreMessage(QtWarningMsg, "theme test:4: accnet is not a theme token, skipped");
        QTest::ignoreMessage(QtWarningMsg, "theme test:5: dark.subtleHover is not a theme token, skipped");
        QTest::ignoreMessage(QtWarningMsg, "theme test:6: not a key=value line, skipped");
        QTest::ignoreMessage(QtWarningMsg, "theme test:7: not a key=value line, skipped");
        QTest::ignoreMessage(QtWarningMsg, "theme test:8: radiusCard is a number, not \"inf\": skipped");
        const vela::themes::File file = read("accent=red\ntext=#12345\nradius=#FF00FF\naccnet=#ffffff\n"
                                             "dark.subtleHover=#ffffff\nradius\n=4\nradiusCard=inf\ncard=#ffffff\n");
        QCOMPARE(file.tokens.keys(), QStringList { QStringLiteral("card") });
        QCOMPARE(kinds(file.problems),
            (QStringList { "expects-color", "expects-color", "expects-number", "unknown-token", "unknown-token",
                "not-key-value", "not-key-value", "expects-number" }));
        QCOMPARE(file.problems[3].line, 4);
        QCOMPARE(file.problems[3].key, QStringLiteral("accnet"));
        const QVariantMap variant = vela::themes::toVariant(file.problems[0]);
        QCOMPARE(variant.value(QStringLiteral("kind")).toString(), QStringLiteral("expects-color"));
        QCOMPARE(variant.value(QStringLiteral("value")).toString(), QStringLiteral("red"));
    }

    // The same lookup as Theme.qml's themed(): the mode's key, then the shared one.
    void themedLookup()
    {
        const QVariantMap map = read("accent=#111111\ndark.accent=#222222\nlight.background=#333333\n").tokens;
        QCOMPARE(vela::themes::themed(map, QStringLiteral("accent"), false).value<QColor>(), QColor(0x22, 0x22, 0x22));
        QCOMPARE(vela::themes::themed(map, QStringLiteral("accent"), true).value<QColor>(), QColor(0x11, 0x11, 0x11));
        QCOMPARE(vela::themes::themed(map, QStringLiteral("background"), true).value<QColor>(), QColor(0x33, 0x33, 0x33));
        QVERIFY(!vela::themes::themed(map, QStringLiteral("background"), false).isValid());
    }

    void configuredNameIsTheLastOne()
    {
        const QString conf = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vela/vela.conf");
        QCOMPARE(vela::themes::configuredName(conf), QStringLiteral("vela")); // no file: Vela Official
        write(conf, "language=it\ntheme=first\nscreen-off=5\ntheme= second \n");
        QCOMPARE(vela::themes::configuredName(conf), QStringLiteral("second"));
        write(conf, "theme=\n");
        QCOMPARE(vela::themes::configuredName(conf), QStringLiteral("vela"));
    }

    void namesStayInTheThemesDirectory()
    {
        write(qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vela/secret.conf"), "text=#ff0000\n");
        for (const char* name : { "", "../secret", "a/b", ".hidden", "/etc/passwd", "a b" }) {
            QVERIFY2(!vela::themes::validName(QLatin1String(name)), name);
            QCOMPARE(vela::themes::locate(QLatin1String(name)), QString());
        }
    }

    void theUsersThemeWinsAndVelaComesFirst()
    {
        write(data() + QStringLiteral("mine.conf"), "# Installed: one\n");
        write(data() + QStringLiteral("other.conf"), "no title line\n");
        write(data() + QStringLiteral("vela.conf"), "# Vela Official: Vela's own look\n# Original author: Brando Giuffrida\n");
        QCOMPARE(vela::themes::locate(QStringLiteral("mine")), data() + QStringLiteral("mine.conf"));
        write(config() + QStringLiteral("mine.conf"), "# Mine: the user's copy\n# Original author: me\n");
        QCOMPARE(vela::themes::locate(QStringLiteral("mine")), config() + QStringLiteral("mine.conf"));
        QCOMPARE(vela::themes::locate(QStringLiteral("missing")), QString());

        const QVariantList list = vela::themes::available();
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("vela"));
        QCOMPARE(list[0].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Vela Official"));
        QCOMPARE(list[0].toMap().value(QStringLiteral("author")).toString(), QStringLiteral("Brando Giuffrida"));
        QCOMPARE(list[1].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Mine")); // not "Installed"
        QCOMPARE(list[1].toMap().value(QStringLiteral("author")).toString(), QStringLiteral("me"));
        QCOMPARE(list[2].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("other"));
        QCOMPARE(list[2].toMap().value(QStringLiteral("author")).toString(), QString());
    }

    // A theme's tokens over its parent's, over the grandparent's.
    void inheritsResolvesParentsFirst()
    {
        write(data() + QStringLiteral("base.conf"), "accent=#112233\nradius=4\ncard=#010101\n");
        write(data() + QStringLiteral("middle.conf"), "schema-version=1\ninherits=base\nradius=6\n");
        write(config() + QStringLiteral("custom.conf"), "inherits=middle\naccent=#AABBCC\n");
        const vela::themes::Resolved resolved = vela::themes::resolve(QStringLiteral("custom"));
        QVERIFY(resolved.problems.isEmpty());
        QCOMPARE(resolved.files,
            (QStringList { config() + QStringLiteral("custom.conf"), data() + QStringLiteral("middle.conf"),
                data() + QStringLiteral("base.conf") }));
        QCOMPARE(resolved.tokens.value(QStringLiteral("accent")).value<QColor>(), QColor(0xAA, 0xBB, 0xCC));
        QCOMPARE(resolved.tokens.value(QStringLiteral("radius")).toDouble(), 6.0);
        QCOMPARE(resolved.tokens.value(QStringLiteral("card")).value<QColor>(), QColor(1, 1, 1));
        QCOMPARE(resolved.tokens.size(), 3); // no inherits, no schema-version
    }

    // The user's vela.conf can start from the installed one of the same name.
    void aUsersThemeInheritsTheInstalledOneOfItsName()
    {
        write(data() + QStringLiteral("vela.conf"), "# Vela Official: Vela's own look\nradius=4\n");
        write(config() + QStringLiteral("vela.conf"), "inherits=vela\naccent=#AABBCC\n");
        const vela::themes::Resolved resolved = vela::themes::resolve(QStringLiteral("vela"));
        QVERIFY(resolved.problems.isEmpty());
        QCOMPARE(resolved.files.size(), 2);
        QCOMPARE(resolved.tokens.value(QStringLiteral("radius")).toDouble(), 4.0);
        QCOMPARE(resolved.tokens.value(QStringLiteral("accent")).value<QColor>(), QColor(0xAA, 0xBB, 0xCC));
    }

    // A wrong inherits= keeps the theme's own tokens and says why.
    void brokenChainsKeepTheThemesOwnTokens()
    {
        write(data() + QStringLiteral("one.conf"), "inherits=two\ntext=#AABBCC\n");
        write(data() + QStringLiteral("two.conf"), "inherits=one\nradius=10\n");
        QTest::ignoreMessage(QtWarningMsg, qPrintable(QStringLiteral("theme %1two.conf:1: inherits=one, which inherits back from this theme: the chain stops here").arg(data())));
        vela::themes::Resolved resolved = vela::themes::resolve(QStringLiteral("one"));
        QCOMPARE(resolved.tokens.value(QStringLiteral("radius")).toDouble(), 10.0);
        QCOMPARE(resolved.tokens.value(QStringLiteral("text")).value<QColor>(), QColor(0xAA, 0xBB, 0xCC));
        QCOMPARE(kinds(resolved.problems), QStringList { "inheritance-cycle" });

        write(data() + QStringLiteral("lone.conf"), "\ninherits=missing\nradius=4\n");
        QTest::ignoreMessage(QtWarningMsg, qPrintable(QStringLiteral("theme %1lone.conf:2: inherits=missing, which is not installed: inherits nothing").arg(data())));
        resolved = vela::themes::resolve(QStringLiteral("lone"));
        QCOMPARE(resolved.tokens.value(QStringLiteral("radius")).toDouble(), 4.0);
        QCOMPARE(kinds(resolved.problems), QStringList { "missing-parent" });
        QCOMPARE(resolved.problems[0].line, 2);

        write(data() + QStringLiteral("odd.conf"), "inherits=../secret\nradius=4\n");
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("inherits=\\.\\./secret is not a theme name")));
        QCOMPARE(kinds(vela::themes::resolve(QStringLiteral("odd")).problems), QStringList { "invalid-parent" });
    }

    void chainsStopAtTheLimit()
    {
        for (int i = 0; i < 20; ++i) {
            write(data() + QStringLiteral("t%1.conf").arg(i), QStringLiteral("inherits=t%1\nradius=%1\n").arg(i + 1).toUtf8());
        }
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("chain of more than 16 themes")));
        const vela::themes::Resolved resolved = vela::themes::resolve(QStringLiteral("t0"));
        QCOMPARE(resolved.files.size(), vela::themes::maxChain);
        QCOMPARE(resolved.tokens.value(QStringLiteral("radius")).toDouble(), 1.0);
        QCOMPARE(kinds(resolved.problems), QStringList { "chain-too-long" });
    }

    // A file for another format is not used: not its tokens, not its parents.
    void anotherSchemaIsNotUsed()
    {
        write(data() + QStringLiteral("base.conf"), "radius=4\n");
        write(data() + QStringLiteral("future.conf"), "schema-version=2\ninherits=base\naccent=#112233\n");
        QTest::ignoreMessage(QtWarningMsg, qPrintable(QStringLiteral("theme %1future.conf:1: schema-version=2, and this Vela reads 1: the file is not used").arg(data())));
        vela::themes::Resolved resolved = vela::themes::resolve(QStringLiteral("future"));
        QVERIFY(resolved.tokens.isEmpty());
        QVERIFY(resolved.files.isEmpty());
        QCOMPARE(kinds(resolved.problems), QStringList { "unsupported-schema" });

        // A parent's: the child keeps its own.
        write(config() + QStringLiteral("child.conf"), "inherits=future\nradius=8\n");
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("schema-version=2")));
        resolved = vela::themes::resolve(QStringLiteral("child"));
        QCOMPARE(resolved.tokens.keys(), QStringList { QStringLiteral("radius") });
        QCOMPARE(resolved.files.size(), 1);
    }

    void aMissingThemeIsVelasOwnLook()
    {
        const vela::themes::Resolved resolved = vela::themes::resolve(QStringLiteral("missing"));
        QVERIFY(resolved.tokens.isEmpty());
        QVERIFY(resolved.files.isEmpty());
        QVERIFY(resolved.problems.isEmpty());
    }

    // Every theme in themes/ resolves without a problem and names its author;
    // Vela Official is there and sets no token.
    void shippedThemesAreClean()
    {
        const QDir themes(QStringLiteral(THEMES_DIR));
        const QStringList files = themes.entryList({ QStringLiteral("*.conf") }, QDir::Files);
        QVERIFY(files.contains(QStringLiteral("vela.conf")));
        QTest::failOnWarning(QRegularExpression(QStringLiteral("^theme ")));
        for (const QString& name : files) {
            QFile in(themes.filePath(name));
            QVERIFY(in.open(QIODevice::ReadOnly | QIODevice::Text));
            const QByteArray text = in.readAll();
            QVERIFY2(text.contains("\n# Original author: ") || text.startsWith("# Original author: "), qPrintable(name));
            QBuffer buffer;
            buffer.setData(text);
            buffer.open(QIODevice::ReadOnly | QIODevice::Text);
            const vela::themes::File file = vela::themes::read(buffer, name);
            QVERIFY2(file.problems.isEmpty(), qPrintable(name));
            if (name == QLatin1String("vela.conf")) {
                QVERIFY(file.tokens.isEmpty());
                QVERIFY(file.parent.isEmpty());
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
