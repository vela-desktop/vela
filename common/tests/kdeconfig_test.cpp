// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela::kde (common/src/kdeconfig.h): KDE's configuration files read and
// written as KConfig does, and the D-Bus messages KDE apps listen to.

#include "kdeconfig.h"

#include <QByteArrayList>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

namespace {

QByteArray contents(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool put(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

// Records the D-Bus signals it is connected to.
class Recorder : public QObject {
    Q_OBJECT
public:
    QList<QDBusMessage> messages;
public slots:
    void record(const QDBusMessage& message) { messages.append(message); }
};

} // namespace

class KdeConfigTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString& name = QStringLiteral("kdeglobals")) const { return m_dir.filePath(name); }

private slots:
    void init() { QFile::remove(path()); }

    // Plasma's own kdeglobals: the font is one comma-separated value in the
    // real [General] section (QSettings read it as a list, and as
    // "%General").
    void readsCommaValuesInGeneral()
    {
        QVERIFY(put(path(),
            "[General]\nfont=Noto Sans,14,-1,5,700,0,0,0,0,0\nmenuFont = Inter,10,-1,5,400,0,0,0,0,0 \n\n"
            "[WM]\nactiveFont=Noto Sans,11,-1,5,700,0,0,0,0,0\n"));
        QCOMPARE(vela::kde::read(path(), QStringLiteral("General"), QStringLiteral("font")),
            QStringLiteral("Noto Sans,14,-1,5,700,0,0,0,0,0"));
        QCOMPARE(vela::kde::read(path(), QStringLiteral("General"), QStringLiteral("menuFont")),
            QStringLiteral("Inter,10,-1,5,400,0,0,0,0,0"));
        QCOMPARE(vela::kde::read(path(), QStringLiteral("WM"), QStringLiteral("activeFont")),
            QStringLiteral("Noto Sans,11,-1,5,700,0,0,0,0,0"));
        QCOMPARE(vela::kde::read(path(), QStringLiteral("WM"), QStringLiteral("font")), std::nullopt);
        QCOMPARE(vela::kde::read(path(), QStringLiteral("Icons"), QStringLiteral("Theme")), std::nullopt);
        QCOMPARE(vela::kde::read(path(QStringLiteral("missing")), QStringLiteral("General"), QStringLiteral("font")),
            std::nullopt);
    }

    void readsAsKConfigDoes()
    {
        QVERIFY(put(path(),
            "# a comment\n[Icons]\nTheme=first\nTheme[it]=localized\nTheme=Papirus\\sDark\\s\n"
            "[Icons][Sub]\nTheme=subgroup\n"
            "[Mouse][$i]\ncursorTheme[$i]=locked\n"
            "[Deleted]\nkey[$d]=\n"
            "[Escapes]\nkey=a\\\\b\\tc\\x41\\q\n"));
        // The last line of the key wins; a locale and a subgroup are not it.
        QCOMPARE(
            vela::kde::read(path(), QStringLiteral("Icons"), QStringLiteral("Theme")), QStringLiteral("Papirus Dark "));
        // Immutable is still readable; deleted is missing.
        QCOMPARE(
            vela::kde::read(path(), QStringLiteral("Mouse"), QStringLiteral("cursorTheme")), QStringLiteral("locked"));
        QCOMPARE(vela::kde::read(path(), QStringLiteral("Deleted"), QStringLiteral("key")), std::nullopt);
        QCOMPARE(
            vela::kde::read(path(), QStringLiteral("Escapes"), QStringLiteral("key")), QStringLiteral("a\\b\tcA\\q"));
    }

    void escapesRoundTrip()
    {
        const QStringList values { QStringLiteral(" leading and trailing "), QStringLiteral("back\\slash"),
            QStringLiteral("tab\tnew\nline\rcr"), QStringLiteral("bell\x07"), QStringLiteral("My,Cursor;Theme"),
            QStringLiteral("Noto Sans,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1") };
        for (const QString& value : values) {
            QCOMPARE(vela::kde::unescape(vela::kde::escape(value)), value);
            QVERIFY(vela::kde::write(path(), { { QStringLiteral("General"), QStringLiteral("key"), value } }));
            QCOMPARE(vela::kde::read(path(), QStringLiteral("General"), QStringLiteral("key")), value);
        }
        // Commas stay as they are: no quotes for KDE to keep.
        QCOMPARE(vela::kde::escape(QStringLiteral("My,Cursor")), QStringLiteral("My,Cursor"));
        QCOMPARE(vela::kde::escape(QStringLiteral(" a ")), QStringLiteral("\\sa\\s"));
    }

    // Everything else stays byte for byte: other groups, comments, locale
    // keys, flags, the order of the lines.
    void writeKeepsTheRestOfTheFile()
    {
        const QByteArray before = "# kept\n[General]\nColorScheme=BreezeDark\nfont=Old,10,-1,5,400,0,0,0,0,0\n"
                                  "font[it]=Localized\n\n[KDE]\nSingleClick=false\n";
        QVERIFY(put(path(), before));
        QVERIFY(vela::kde::write(path(),
            { { QStringLiteral("General"), QStringLiteral("font"), QStringLiteral("New,10,-1,5,400,0,0,0,0,0") } }));
        QCOMPARE(contents(path()),
            QByteArray("# kept\n[General]\nColorScheme=BreezeDark\nfont=New,10,-1,5,400,0,0,0,0,0\n"
                       "font[it]=Localized\n\n[KDE]\nSingleClick=false\n"));
    }

    void writeAddsMissingKeysAndGroups()
    {
        QVERIFY(put(path(), "[General]\nColorScheme=BreezeDark\n\n[KDE]\nSingleClick=false\n"));
        QVERIFY(vela::kde::write(path(),
            { { QStringLiteral("General"), QStringLiteral("menuFont"), QStringLiteral("A,9") },
                { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("breeze") } }));
        // At the end of its group (before the blank line), or a new group at
        // the end of the file.
        QCOMPARE(contents(path()),
            QByteArray("[General]\nColorScheme=BreezeDark\nmenuFont=A,9\n\n[KDE]\nSingleClick=false\n\n"
                       "[Icons]\nTheme=breeze\n"));
    }

    void writeCreatesTheFile()
    {
        const QString nested = m_dir.filePath(QStringLiteral("new/dir/kcminputrc"));
        QVERIFY(vela::kde::write(nested,
            { { QStringLiteral("Mouse"), QStringLiteral("cursorTheme"), QStringLiteral("Bibata-Modern-Ice") } }));
        QCOMPARE(contents(nested), QByteArray("[Mouse]\ncursorTheme=Bibata-Modern-Ice\n"));
        QVERIFY(!QFile::exists(nested + QStringLiteral(".lock")));
    }

    void writeRewritesEveryLineOfTheKey()
    {
        QVERIFY(put(path(), "[Icons]\nTheme=a\n[Other]\nTheme=x\n[Icons]\nTheme=b\n"));
        QVERIFY(
            vela::kde::write(path(), { { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("c") } }));
        QCOMPARE(contents(path()), QByteArray("[Icons]\nTheme=c\n[Other]\nTheme=x\n[Icons]\nTheme=c\n"));
    }

    // KDE's immutable marks ([$i] on the file, the group or the key): the
    // administrator's choice. Nothing is written, not even the other
    // entries of the same call.
    void immutableIsLeftAlone_data()
    {
        QTest::addColumn<QByteArray>("before");
        QTest::newRow("file") << QByteArray("[$i]\n[Icons]\nTheme=a\n");
        QTest::newRow("group") << QByteArray("[Icons][$i]\nTheme=a\n");
        QTest::newRow("key") << QByteArray("[Icons]\nTheme[$i]=a\n");
    }

    void immutableIsLeftAlone()
    {
        QFETCH(QByteArray, before);
        QVERIFY(put(path(), before));
        QVERIFY(!vela::kde::write(path(),
            { { QStringLiteral("General"), QStringLiteral("font"), QStringLiteral("A") },
                { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("b") } }));
        QCOMPARE(contents(path()), before);
    }

    void failedWriteLeavesTheFile()
    {
        if (geteuid() == 0) {
            QSKIP("Root bypasses Unix permissions.");
        }
        const QByteArray before = "[Icons]\nTheme=a\n";
        QVERIFY(put(path(), before));
        QVERIFY(QFile::setPermissions(path(), QFileDevice::WriteOwner)); // unreadable
        QVERIFY(
            !vela::kde::write(path(), { { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("b") } }));
        QVERIFY(QFile::setPermissions(path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(contents(path()), before);

        // A busy lock (another writer): not written, never waited on long.
        QLockFile held(path() + QStringLiteral(".lock"));
        QVERIFY(held.lock());
        QVERIFY(
            !vela::kde::write(path(), { { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("b") } }));
        held.unlock();
        QCOMPARE(contents(path()), before);
    }

    // Dotfile managers link kdeglobals into a repository; its mode stays.
    void writeFollowsLinksAndKeepsTheMode()
    {
        const QString target = m_dir.filePath(QStringLiteral("dotfiles/kdeglobals"));
        QVERIFY(put(target, "[Icons]\nTheme=a\n"));
        QVERIFY(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        const QString link = path(QStringLiteral("linked"));
        QVERIFY(QFile::link(target, link));
        QVERIFY(vela::kde::write(link, { { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral("b") } }));
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(contents(target), QByteArray("[Icons]\nTheme=b\n"));
        const QFileDevice::Permissions others =
            QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther;
        QCOMPARE(QFile::permissions(target) & others, QFileDevice::Permissions());
    }

    // KDE's own reader agrees, when it is installed (not a round trip
    // through our reader).
    void kreadconfigReadsWhatIsWritten()
    {
        const QString kread = QStandardPaths::findExecutable(QStringLiteral("kreadconfig6"));
        if (kread.isEmpty()) {
            QSKIP("kreadconfig6 is not installed.");
        }
        const QString font = QStringLiteral("My, Font,13,-1,5,700,0,0,0,0,0,0,0,0,0,0,1");
        QVERIFY(put(path(), "[General]\nColorScheme=BreezeDark\n"));
        QVERIFY(vela::kde::write(path(),
            { { QStringLiteral("General"), QStringLiteral("font"), font },
                { QStringLiteral("Icons"), QStringLiteral("Theme"), QStringLiteral(" Spaced Theme ") } }));
        const auto kreadconfig = [&](const QString& group, const QString& key) {
            QProcess process;
            process.start(kread,
                { QStringLiteral("--file"), path(), QStringLiteral("--group"), group, QStringLiteral("--key"), key });
            process.waitForFinished();
            return QString::fromUtf8(process.readAllStandardOutput()).chopped(1); // its newline
        };
        QCOMPARE(kreadconfig(QStringLiteral("General"), QStringLiteral("font")), font);
        QCOMPARE(kreadconfig(QStringLiteral("Icons"), QStringLiteral("Theme")), QStringLiteral(" Spaced Theme "));
        QCOMPARE(kreadconfig(QStringLiteral("General"), QStringLiteral("ColorScheme")), QStringLiteral("BreezeDark"));
    }

    // The messages KDE apps listen to, as KConfig and Plasma's settings
    // modules send them.
    void notifiesKdeApps()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            QSKIP("No session bus (the test runs under dbus-run-session in ctest).");
        }
        Recorder recorder;
        QVERIFY(bus.connect(QString(), QStringLiteral("/kdeglobals"), QStringLiteral("org.kde.kconfig.notify"),
            QStringLiteral("ConfigChanged"), &recorder, SLOT(record(QDBusMessage))));
        QVERIFY(bus.connect(QString(), QStringLiteral("/KGlobalSettings"), QStringLiteral("org.kde.KGlobalSettings"),
            QStringLiteral("notifyChange"), &recorder, SLOT(record(QDBusMessage))));
        QVERIFY(bus.connect(QString(), QStringLiteral("/KDEPlatformTheme"), QStringLiteral("org.kde.KDEPlatformTheme"),
            QStringLiteral("refreshFonts"), &recorder, SLOT(record(QDBusMessage))));
        QVERIFY(bus.connect(QString(), QStringLiteral("/KIconLoader"), QStringLiteral("org.kde.KIconLoader"),
            QStringLiteral("iconChanged"), &recorder, SLOT(record(QDBusMessage))));

        vela::kde::notifyChanged(path(),
            { { QStringLiteral("General"), QStringLiteral("font"), QString() },
                { QStringLiteral("General"), QStringLiteral("menuFont"), QString() },
                { QStringLiteral("WM"), QStringLiteral("activeFont"), QString() } });
        vela::kde::notifyGlobalChange(vela::kde::GlobalChange::Icon);
        vela::kde::refreshFonts();
        vela::kde::iconLoaderChanged();
        QTRY_COMPARE(recorder.messages.size(), 4);

        const QDBusMessage changed = recorder.messages[0];
        QCOMPARE(changed.member(), QStringLiteral("ConfigChanged"));
        QCOMPARE(changed.signature(), QStringLiteral("a{saay}"));
        const auto changes = qdbus_cast<QHash<QString, QByteArrayList>>(changed.arguments().constFirst());
        QCOMPARE(changes.value(QStringLiteral("General")), (QByteArrayList { "font", "menuFont" }));
        QCOMPARE(changes.value(QStringLiteral("WM")), (QByteArrayList { "activeFont" }));
        QCOMPARE(recorder.messages[1].arguments(), (QVariantList { 4, 0 }));
        QCOMPARE(recorder.messages[2].member(), QStringLiteral("refreshFonts"));
        QCOMPARE(recorder.messages[3].arguments(), (QVariantList { 0 }));

        // A file name that is no D-Bus path element sends nothing.
        vela::kde::notifyChanged(path(QStringLiteral("kde-globals.rc")),
            { { QStringLiteral("General"), QStringLiteral("font"), QString() } });
        vela::kde::refreshFonts();
        QTRY_COMPARE(recorder.messages.size(), 5);
        QCOMPARE(recorder.messages[4].member(), QStringLiteral("refreshFonts"));
    }
};

QTEST_GUILESS_MAIN(KdeConfigTest)
#include "kdeconfig_test.moc"
