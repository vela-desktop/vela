// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "servicemenus.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

/** Write a fixture under the private service-menu data tree. */
static bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

class ServiceMenusTest : public QObject {
    Q_OBJECT
    QString selected;

    /** Find a named fixture action without assuming its position in the menu. */
    int actionId(ServiceMenus& menus, const QString& name) const
    {
        for (const QVariant& action : menus.actionsFor({ selected })) {
            const QVariantMap entry = action.toMap();
            if (entry.value(QStringLiteral("text")).toString() == name) {
                return entry.value(QStringLiteral("id")).toInt();
            }
        }
        return -1;
    }

private slots:
    /** Install invalid expansion cases and a real child-process recorder. */
    void initTestCase()
    {
        const QString root = qEnvironmentVariable("XDG_DATA_HOME");
        selected = root + QStringLiteral("/selected files/a document.txt");
        QVERIFY(writeFile(selected, "selection"));
        const QByteArray desktop =
            "[Desktop Entry]\nType=Service\nMimeType=all/all;\nActions=missing;deprecated;blank;record;\n"
            "[Desktop Action missing]\nName=Missing\nExec=%k\n"
            "[Desktop Action deprecated]\nName=Deprecated\nExec=%d\n"
            "[Desktop Action blank]\nName=Blank\nExec=\"\"\n"
            "[Desktop Action record]\nName=Record\nExec=\""
            + QCoreApplication::applicationFilePath().toUtf8() + "\" --record %F\n";
        QVERIFY(writeFile(root + QStringLiteral("/kio/servicemenus/fixture.desktop"), desktop));
    }

    /** Cover missing context, removed field codes and a quoted empty program. */
    void emptyExpansions_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("missing-context") << QStringLiteral("Missing");
        QTest::newRow("deprecated-code") << QStringLiteral("Deprecated");
        QTest::newRow("empty-program") << QStringLiteral("Blank");
    }

    /** Reject invalid commands without dereferencing an empty argument list. */
    void emptyExpansions()
    {
        QFETCH(QString, name);
        ServiceMenus menus;
        const int id = actionId(menus, name);
        QVERIFY(id >= 0);
        QTest::ignoreMessage(QtWarningMsg, "vela-shell: service menu Exec expanded to an empty command");
        menus.run(id, { selected });
        QVERIFY(!QFile::exists(qEnvironmentVariable("VELA_SERVICE_MENU_PROBE")));
    }

    /** Valid expansions keep the selected path and its directory intact. */
    void validCommandStillLaunches()
    {
        ServiceMenus menus;
        const int id = actionId(menus, QStringLiteral("Record"));
        QVERIFY(id >= 0);
        menus.run(id, { selected });
        const auto payload = [] {
            QFile file(qEnvironmentVariable("VELA_SERVICE_MENU_PROBE"));
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        QTRY_VERIFY_WITH_TIMEOUT(!payload().isEmpty(), 5000);
        const QJsonObject result = QJsonDocument::fromJson(payload()).object();
        QCOMPARE(result.value(QStringLiteral("paths")).toArray(), QJsonArray { selected });
        QCOMPARE(result.value(QStringLiteral("directory")).toString(), QFileInfo(selected).absolutePath());
    }
};

/** Run the isolated suite or record a detached service-menu child's arguments. */
int main(int argc, char** argv)
{
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--record")) {
        QCoreApplication app(argc, argv);
        QFile output(qEnvironmentVariable("VELA_SERVICE_MENU_PROBE"));
        const QJsonObject payload {
            { QStringLiteral("paths"), QJsonArray::fromStringList(app.arguments().mid(2)) },
            { QStringLiteral("directory"), QDir::currentPath() }
        };
        return output.open(QIODevice::WriteOnly)
                && output.write(QJsonDocument(payload).toJson(QJsonDocument::Compact)) > 0
            ? 0 : 1;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return 1;
    }
    qputenv("XDG_DATA_HOME", directory.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_DATA_DIRS", directory.filePath(QStringLiteral("system-data")).toUtf8());
    qputenv("VELA_SERVICE_MENU_PROBE", directory.filePath(QStringLiteral("probe.json")).toUtf8());
    QCoreApplication app(argc, argv);
    ServiceMenusTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "servicemenus.moc"
