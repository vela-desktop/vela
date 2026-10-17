// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-files: Vela's File Explorer, like Windows 11's.
//
//   vela-files [folder or file...]   (a file: its folder opens, with the file selected)
//
// Goal: a cold start under 150 ms (README, "Measurable goals"). That's why
// there are no Qt Quick Controls, and what the first frame doesn't need (apps
// for "Open with", service menus) is prepared afterwards. With
// VELA_FILES_TIMING=1 it prints how long it took.

#include "appmodel.h"
#include "../../common/filetransferrequest.h"
#include "controls.h"
#include "fileops.h"
#include "language.h"
#include "filethumbnails.h"
#include "foldermodel.h"
#include "iconprovider.h"
#include "places.h"
#include "servicemenus.h"
#include "systemactions.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <QtQml/QQmlExtensionPlugin>

#include <ctime>
#include <unistd.h>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

namespace {

// How long since the process started (ms), from /proc/self/stat.
double sinceProcessStart()
{
    QFile stat(QStringLiteral("/proc/self/stat"));
    if (!stat.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QByteArray line = stat.readAll();
    const QList<QByteArray> fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
    if (fields.size() < 20) {
        return -1;
    }
    const double startTicks = fields.at(19).toDouble(); // field 22 of stat
    timespec now {};
    clock_gettime(CLOCK_BOOTTIME, &now);
    const double ticks = double(sysconf(_SC_CLK_TCK));
    return (now.tv_sec + now.tv_nsec / 1e9 - startTicks / ticks) * 1000.0;
}

} // namespace

int main(int argc, char* argv[])
{
    QElapsedTimer sinceMain;
    sinceMain.start();
    const bool timing = qEnvironmentVariableIntValue("VELA_FILES_TIMING") > 0;

    // Text with FreeType and font hinting, like Plasma and Windows: crisper
    // than Qt Quick's default text (distance fields, no hinting), on the
    // output's real pixels even at fractional scales.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_DEFAULT_TEXT_RENDER_TYPE")) {
        QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-files"));
    QQmlApplicationEngine* qmlEngine = nullptr;
    vela::language::install(QStringLiteral("vela-files"), [&qmlEngine] {
        QGuiApplication::setApplicationDisplayName(QCoreApplication::translate("Files", "File Explorer"));
        if (qmlEngine) {
            qmlEngine->retranslate();
        }
    });
    QGuiApplication::setApplicationDisplayName(QCoreApplication::translate("Files", "File Explorer"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    QGuiApplication::setDesktopFileName(QStringLiteral("vela-files"));
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("system-file-manager")));

    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));

    // Where to open: the given folder, or the given file's folder (selected).
    QString start = QStringLiteral("home:");
    QString select;
    const QStringList args = app.arguments().mid(1);
    vela::files::TransferRequest externalTransfer;
    const bool transferMode = vela::files::parseTransferArguments(args, &externalTransfer);
    if (!args.isEmpty() && args.first() == QLatin1String("--vela-transfer") && !transferMode) {
        qCritical("vela-files: invalid transfer request");
        return 2;
    }
    if (transferMode && !QFileInfo(externalTransfer.directory).isDir()) {
        qCritical("vela-files: destination folder does not exist");
        return 2;
    }
    if (transferMode) {
        start = QFileInfo(externalTransfer.directory).absoluteFilePath();
    } else if (!args.isEmpty()) {
        const QUrl url = QUrl::fromUserInput(args.first(), QDir::currentPath(), QUrl::AssumeLocalFile);
        const QFileInfo info(url.isLocalFile() ? url.toLocalFile() : args.first());
        if (info.isDir()) {
            start = info.absoluteFilePath();
        } else if (info.exists()) {
            start = info.absolutePath();
            select = info.absoluteFilePath();
        }
    }

    qmlRegisterType<FolderModel>("Vela.Files.Backend", 1, 0, "FolderModel");

    AppModel apps;
    FileOps ops(&apps);
    Places places;
    places.setTransferGuard([&ops](const QString& device) { return ops.activeOnDevice(device); });
    ops.setEjectGuard([&places](const QStringList& sources, const QString& destination) {
        for (const QString& device : places.ejectingDevices()) {
            if (FileOps::pathsOnDevice(sources, destination, device)) {
                return true;
            }
        }
        return false;
    });
    SystemActions system;
    ServiceMenus serviceMenus;

    QQmlApplicationEngine engine;
    qmlEngine = &engine;
    // Theme and Style (the apps' mode, the accent and Mica from the shell;
    // icons from the matching theme, breeze or breeze-dark): Vela.Controls.
    Appearance* style = vela::controls::install(engine, Appearance::Mode::Apps);
    engine.addImageProvider(QStringLiteral("fileicon"), new IconProvider(32));
    engine.addImageProvider(QStringLiteral("filethumb"), new FileThumbnailProvider);
    QQmlContext* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("Apps"), &apps);
    context->setContextProperty(QStringLiteral("Ops"), &ops);
    context->setContextProperty(QStringLiteral("Places"), &places);
    context->setContextProperty(QStringLiteral("System"), &system);
    context->setContextProperty(QStringLiteral("ServiceMenus"), &serviceMenus);
    context->setContextProperty(QStringLiteral("StartLocation"), start);
    context->setContextProperty(QStringLiteral("StartSelection"), select);
    engine.loadFromModule("Vela.Files", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    // A desktop paste/drop uses exactly the same FileOps requestTransfer()
    // pipeline as Explorer's own paste/drop: conflict UI, progress and cancel.
    // Dispatch only after the QML window has connected to Ops signals.
    if (transferMode) {
        QStringList urls;
        for (const QString& source : externalTransfer.sources) {
            const QFileInfo info(source);
            if (info.exists()) {
                urls.append(QUrl::fromLocalFile(info.absoluteFilePath()).toString());
            }
        }
        if (urls.isEmpty()) {
            qCritical("vela-files: no valid source files for transfer");
            return 2;
        }
        QTimer::singleShot(0, &ops, [&ops, urls, externalTransfer] {
            ops.drop(urls, externalTransfer.directory, static_cast<int>(externalTransfer.action));
        });
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window) {
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&, timing] {
            static bool first = true;
            if (!first) {
                return;
            }
            first = false;
            if (timing) {
                std::fprintf(stderr, "vela-files: first frame %.0f ms after main, %.0f ms after process start\n",
                    double(sinceMain.nsecsElapsed()) / 1e6, sinceProcessStart());
            }
            // What the first frame doesn't need.
            QTimer::singleShot(0, &app, [&apps, style] {
                apps.reload();
                style->watch();
            });
        }, Qt::QueuedConnection);
    }
    return app.exec();
}

