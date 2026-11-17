// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Fixed native Wayland scene. The launcher entry starts this same installed
// binary with --launch; the receipt is functional evidence, not a latency clock.
#include <QGuiApplication>
#include <QWindow>
#include <QBackingStore>
#include <QPainter>
#include <QMouseEvent>
#include <QTimer>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <ctime>
#include <QKeyEvent>
#include <cstdio>
#include <unistd.h>

static constexpr quint32 sceneId = 0x715000, launchedId = 0x715001,peerId=0x715002;
static bool observing;
static quint64 now() {timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return quint64(t.tv_sec)*1000000000ull+t.tv_nsec;}
static void emitState(QJsonObject value) {const auto text=QJsonDocument(value).toJson(QJsonDocument::Compact);std::fwrite(text.constData(),1,text.size(),stdout);std::putchar('\n');std::fflush(stdout);}
class Scene : public QWindow {
    QBackingStore store{this};
    quint32 id;
    QJsonObject previous;quint64 sequence=0;
public:
    explicit Scene(quint32 value) : id(value) {
        setFlags(Qt::Window | Qt::FramelessWindowHint);
        setTitle(QStringLiteral("Vela benchmark scene"));
        resize(640, 360);
        if(observing)setMinimumSize(QSize(352,240));
    }
    void snapshot() {
        QJsonObject value{{"event","scene-state"},{"method","native-desktop-scene-v2"},{"id",qint64(id)},
            {"uid",qint64(getuid())},{"pid",qint64(getpid())},{"protocol","wayland"},{"width",width()},{"height",height()},
            {"active",isActive()},{"maximized",windowState()==Qt::WindowMaximized},{"scale",devicePixelRatio()}};
        if(value==previous)return;
        previous=value;value["sequence"]=qint64(++sequence);value["monotonic_ns"]=QString::number(now());emitState(value);
    }
    bool event(QEvent *event) override {
        if (event->type() == QEvent::Expose || event->type() == QEvent::Resize) paint();
        if (event->type() == QEvent::MouseButtonPress) {
            auto mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && ((mouse->position().y() >= 64 && mouse->position().y() < 128) || (observing && mouse->position().x()>width()-48 && mouse->position().y()>height()-48))) {
                const bool resizing=observing && mouse->position().x()>width()-48 && mouse->position().y()>height()-48;
                const bool accepted = resizing?startSystemResize(Qt::RightEdge|Qt::BottomEdge):startSystemMove();
                if(observing)emitState(QJsonObject{{"event",resizing?"resize-request":"move-request"},{"accepted",accepted},{"monotonic_ns",QString::number(now())},{"id",qint64(id)}});
                else std::printf("{\"event\":\"move-request\",\"accepted\":%s}\n", accepted ? "true" : "false");
                std::fflush(stdout);
            }
        }
        return QWindow::event(event);
    }
    void paint() {
        if (!isExposed()) return;
        store.resize(size());
        store.beginPaint(QRect(QPoint(), size()));
        QPainter painter(store.paintDevice());
        painter.fillRect(QRect(QPoint(), size()), QColor(32, 40, 48));
        // Same inset marker vocabulary used by the calibrated capture route.
        painter.fillRect(16, 16, 16, 32, QColor(208, 32, 208));
        painter.fillRect(32, 16, 16, 32, QColor(32, 208, 32));
        const int stride = width() < 640 ? 8 : 17;
        for (int bit = 0; bit < 32; ++bit)
            painter.fillRect(48 + bit * stride, 16, stride, 32, (id >> bit) & 1 ? Qt::white : QColor(32,40,48));
        painter.fillRect(48 + 32 * stride, 16, 16, 32, QColor(32,64,224));
        painter.fillRect(64 + 32 * stride, 16, 16, 32, QColor(224,128,32));
        // Keep the system cursor and its shadow away from the encoded bits.
        // Cursor-free requests can still contain a composited software cursor.
        painter.fillRect(0, 64, 640, 64, QColor(44, 58, 68));
        painter.setPen(Qt::white);
        painter.drawText(QRect(0,64,640,64), Qt::AlignCenter, QStringLiteral("Drag here"));
        painter.drawText(QRect(30,144,580,180), Qt::AlignCenter,
                         QStringLiteral("Vela benchmark scene\nNative Wayland • fixed 640 × 360 content"));
        if(observing) {
            painter.fillRect(width()-24,height()-24,16,16,id==peerId?QColor(200,120,80):QColor(200,80,120));
            painter.drawText(QRect(width()-80,height()-60,64,24),Qt::AlignCenter,QStringLiteral("Resize"));
        }
        painter.end();
        store.endPaint();
        store.flush(QRect(QPoint(), size()));
    }
};
int main(int argc, char **argv) {
    if (argc == 2 && QByteArray(argv[1]) == "--help") {
        std::puts("Usage: vela-bench-scene --scene | --launch | --receipt | --observe | --peer");
        return 0;
    }
    if (getuid() == 0 || geteuid() == 0 || argc != 2) return 1;
    const QByteArray operation(argv[1]);
    const auto receipt = qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/vela-benchmark-launch.json");
    if (operation == "--receipt") {
        QFile file(receipt);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return 1;
        const auto bytes = file.readAll();
        std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
        return 0;
    }
    observing=operation=="--observe" || operation=="--peer";
    if (operation != "--scene" && operation != "--launch" && !observing) return 1;
    qputenv("QT_QPA_PLATFORM", "wayland");
    QGuiApplication app(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) return 1;
    QGuiApplication::setDesktopFileName(operation=="--peer"?QStringLiteral("org.vela.BenchmarkPeer"):QStringLiteral("org.vela.BenchmarkScene"));
    if (operation == "--launch") {
        QSaveFile file(receipt);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        file.write(QJsonDocument(QJsonObject{{"schema",1},{"event","launcher-app-started"},
                   {"uid",qint64(getuid())},{"pid",qint64(getpid())},{"marker",qint64(launchedId)}}).toJson());
        if (!file.commit()) return 1;
    }
    Scene window(operation == "--launch" ? launchedId : operation=="--peer"?peerId:sceneId);
    window.show();
    std::puts("{\"event\":\"scene-ready\",\"protocol\":\"wayland\",\"width\":640,\"height\":360}");
    std::fflush(stdout);
    QTimer stateTimer;
    if(observing) {QObject::connect(&stateTimer,&QTimer::timeout,&window,[&]{window.snapshot();});stateTimer.start(50);}
    QTimer::singleShot(180000, &app, &QCoreApplication::quit);
    return app.exec();
}
