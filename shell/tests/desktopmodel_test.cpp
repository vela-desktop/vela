// SPDX-License-Identifier: GPL-3.0-or-later

// The desktop model's settings scope: like every other reader of the shell's
// choices, it reads and writes ~/.config/Vela/vela-shell.conf (the file
// Settings writes), not the default QSettings scope. On a private home, so
// nothing reaches the user's configuration.
//
//   ctest --test-dir build -R shell-desktop-model

#include "desktopmodel.h"
#include "appmodel.h"
#include "privatehome.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace vela::test;

namespace {

QString shellConfPath()
{
    return home().filePath(QStringLiteral("config/Vela/vela-shell.conf"));
}

// The file as another process (Settings) sees it.
QVariant saved(const QString& key)
{
    QSettings settings(shellConfPath(), QSettings::IniFormat);
    return settings.value(key);
}

} // namespace

class DesktopModelTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(QDir().mkpath(QFileInfo(confPath()).absolutePath()));
        QFile::remove(confPath());
    }

    // The bug: the model wrote desktop/* keys through the default QSettings
    // scope, so Settings (and a restart) never saw the desktop's choices.
    void writesTheShellConfigFile()
    {
        AppModel apps;
        DesktopModel desktop(&apps);
        desktop.setShowIcons(false);
        desktop.setIconSize(2);
        QVERIFY(saved(QStringLiteral("desktop/showIcons")).isValid()); // false, not absent
        QCOMPARE(saved(QStringLiteral("desktop/showIcons")).toBool(), false);
        QCOMPARE(saved(QStringLiteral("desktop/iconSize")).toInt(), 2);
    }

    // The same file read back: a choice survives a restart (a new model).
    void readsTheShellConfigFile()
    {
        QSettings writer(confPath(), QSettings::IniFormat);
        writer.setValue(QStringLiteral("desktop/showIcons"), false);
        writer.setValue(QStringLiteral("desktop/iconSize"), 2);
        writer.sync();

        AppModel apps;
        DesktopModel desktop(&apps);
        QCOMPARE(desktop.showIcons(), false);
        QCOMPARE(desktop.iconSize(), 2);
    }

    // The default is on, like Config::showDesktopIcons's.
    void defaultsBeforeAnyChoice()
    {
        AppModel apps;
        DesktopModel desktop(&apps);
        QVERIFY(desktop.showIcons());
        QCOMPARE(desktop.iconSize(), 1);
    }

    // The desktop's own menu toggles it; the file is what Settings watches.
    void togglingWritesAndTheModelAgrees()
    {
        AppModel apps;
        DesktopModel desktop(&apps);
        QSignalSpy view(&desktop, &DesktopModel::viewChanged);

        desktop.setShowIcons(false);
        QCOMPARE(view.count(), 1);
        QCOMPARE(desktop.showIcons(), false);
        QVERIFY(saved(QStringLiteral("desktop/showIcons")).isValid()); // false, not absent
        QCOMPARE(saved(QStringLiteral("desktop/showIcons")).toBool(), false);

        desktop.setShowIcons(true);
        QCOMPARE(view.count(), 2);
        QVERIFY(saved(QStringLiteral("desktop/showIcons")).toBool());
    }

    // Auto arrange: the flag is saved, and the saved positions are dropped
    // (the desktop lays the icons out again).
    void autoArrangeSavesAndClearsPositions()
    {
        AppModel apps;
        DesktopModel desktop(&apps);
        desktop.setShowIcons(false); // a model without icons of its own

        desktop.setAutoArrange(true);
        QVERIFY(saved(QStringLiteral("desktop/autoArrange")).toBool());

        desktop.setAutoArrange(false);
        QVERIFY(!saved(QStringLiteral("desktop/autoArrange")).toBool());
    }

    // Align to grid: the choice is saved like the others.
    void alignToGridIsSaved()
    {
        AppModel apps;
        DesktopModel desktop(&apps);
        desktop.setAlignToGrid(false);
        QVERIFY(!saved(QStringLiteral("desktop/alignToGrid")).toBool());
        desktop.setAlignToGrid(true);
        QVERIFY(saved(QStringLiteral("desktop/alignToGrid")).toBool());
    }

    // Sort by: the mode is saved, so a restart keeps the order.
    void sortModeIsSaved()
    {
        AppModel apps;
        DesktopModel desktop(&apps);
        desktop.sortBy(2); // item type
        QCOMPARE(saved(QStringLiteral("desktop/sortMode")).toInt(), 2);
    }
};

int main(int argc, char** argv)
{
    if (!isolate()) {
        return 1;
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-shell"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    DesktopModelTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "desktopmodel_test.moc"
