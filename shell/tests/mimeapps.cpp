// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests of default apps (mimeapps.h): reading per the freedesktop spec
// and writing that doesn't spoil the rest of the file. All in temporary
// directories (XDG_CONFIG_HOME, XDG_DATA_HOME...).
//
//   ctest --test-dir build -R mimeapps      (or build/shell/vela-mimeapps-test)

#include "mimeapps.h"

#include <QProcess>
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

QString read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

} // namespace

class MimeAppsTest : public QObject {
    Q_OBJECT

    // The temporary directories main() points to with the XDG variables.
    static QString config() { return qEnvironmentVariable("XDG_CONFIG_HOME"); }
    static QString data() { return qEnvironmentVariable("XDG_DATA_HOME"); }

private slots:
    void init()
    {
        QDir(config()).removeRecursively();
        QDir(data()).removeRecursively();
        // The "installed" apps: an empty .desktop is enough to find them.
        for (const char* app : { "kate.desktop", "okular.desktop", "firefox.desktop", "brave.desktop" }) {
            write(data() + QStringLiteral("/applications/") + QLatin1String(app), "[Desktop Entry]\n");
        }
    }

    void writePreservesTheRest()
    {
        write(config() + QStringLiteral("/mimeapps.list"),
            "# a comment\n[Added Associations]\ntext/plain=okular.desktop;\n\n"
            "[Default Applications]\nimage/png=gwenview.desktop;\n\n[Other]\nkey=value\n");
        QVERIFY(MimeApps::setDefault({ QStringLiteral("text/plain") }, QStringLiteral("kate.desktop")));
        const QString text = read(config() + QStringLiteral("/mimeapps.list"));
        QVERIFY(text.contains(QLatin1String("# a comment")));
        QVERIFY(text.contains(QLatin1String("image/png=gwenview.desktop;")));
        QVERIFY(text.contains(QLatin1String("[Other]\nkey=value")));
        // The chosen app first among the associations, the others stay.
        QVERIFY(text.contains(QLatin1String("text/plain=kate.desktop;okular.desktop;")));
        QCOMPARE(MimeApps::defaultFor(QStringLiteral("text/plain")), QStringLiteral("kate.desktop"));
    }

    void createsTheFile()
    {
        QVERIFY(MimeApps::setDefault({ QStringLiteral("application/pdf") }, QStringLiteral("okular.desktop")));
        QCOMPARE(MimeApps::defaultFor(QStringLiteral("application/pdf")), QStringLiteral("okular.desktop"));
        QCOMPARE(MimeApps::addedFor(QStringLiteral("application/pdf")), QStringList { QStringLiteral("okular.desktop") });
    }

    void desktopFileWins()
    {
        // The current desktop's file wins over mimeapps.list (Vela before
        // KDE).
        write(config() + QStringLiteral("/mimeapps.list"), "[Default Applications]\nx-scheme-handler/https=firefox.desktop;\n");
        write(config() + QStringLiteral("/kde-mimeapps.list"), "[Default Applications]\nx-scheme-handler/https=brave.desktop;\n");
        QCOMPARE(MimeApps::defaultFor(QStringLiteral("x-scheme-handler/https")), QStringLiteral("brave.desktop"));
        // Choosing also updates kde-mimeapps.list: the choice really applies.
        QVERIFY(MimeApps::setDefault({ QStringLiteral("x-scheme-handler/https") }, QStringLiteral("firefox.desktop")));
        QCOMPARE(MimeApps::defaultFor(QStringLiteral("x-scheme-handler/https")), QStringLiteral("firefox.desktop"));
        // But a desktop file that didn't exist isn't created.
        QVERIFY(!QFile::exists(config() + QStringLiteral("/vela-mimeapps.list")));
    }

    void aliases()
    {
        // Written with the old name, it's found with the new one too (and vice
        // versa).
        // Explicit test-only MIME database; distribution versions may not agree
        // on the legacy Matroska alias, so never assert it exists system-wide.
        write(config() + QStringLiteral("/mimeapps.list"),
            "[Default Applications]\napplication/x-vela-legacy=kate.desktop;\n");
        QCOMPARE(MimeApps::defaultFor(QStringLiteral("application/x-vela-canonical")), QStringLiteral("kate.desktop"));
        QVERIFY(MimeApps::setDefault({ QStringLiteral("application/x-vela-canonical") }, QStringLiteral("okular.desktop")));
        QCOMPARE(MimeApps::defaultFor(QStringLiteral("application/x-vela-legacy")), QStringLiteral("okular.desktop"));
    }

    void skipsMissingApps()
    {
        write(config() + QStringLiteral("/mimeapps.list"),
            "[Default Applications]\ntext/plain=nonesiste.desktop;kate.desktop;\n");
        QCOMPARE(MimeApps::defaultFor(QStringLiteral("text/plain")), QStringLiteral("kate.desktop"));
    }
};

int main(int argc, char* argv[])
{
    QTemporaryDir dir;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(dir.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(dir.filePath(QStringLiteral("sys-config"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(dir.filePath(QStringLiteral("data"))));
    // After the fake one, the system directories: the type database needs them
    // (/usr/share/mime, for aliases).
    qputenv("XDG_DATA_DIRS", QFile::encodeName(dir.filePath(QStringLiteral("sys-data"))) + ":/usr/local/share:/usr/share");
    qputenv("XDG_CURRENT_DESKTOP", "Vela:KDE");
    // Generate a deterministic MIME database in a private XDG_DATA_DIRS entry.
    // Neither Fedora's nor Arch's system database should define these names.
    const QString mimeDir = dir.filePath(QStringLiteral("sys-data/mime"));
    write(mimeDir + QStringLiteral("/packages/vela-test.xml"),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
        "  <mime-type type=\"application/x-vela-canonical\">\n"
        "    <comment>Vela test type</comment>\n"
        "    <alias type=\"application/x-vela-legacy\"/>\n"
        "  </mime-type>\n"
        "</mime-info>\n");
    if (QProcess::execute(QStringLiteral("update-mime-database"), { mimeDir }) != 0) {
        qCritical("Unable to prepare isolated MIME alias fixture (shared-mime-info missing?)");
        return 1;
    }
    QCoreApplication app(argc, argv);
    MimeAppsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "mimeapps.moc"
