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
#include <cstdio>
#include <unistd.h>

static constexpr quint32 sceneId = 0x715000, launchedId = 0x715001;
class Scene : public QWindow {
    QBackingStore store{this};
    quint32 id;
public:
    explicit Scene(quint32 value) : id(value) {
        setFlags(Qt::Window | Qt::FramelessWindowHint);
        setTitle(QStringLiteral("Vela benchmark scene"));
        resize(640, 360);
    }
    bool event(QEvent *event) override {
        if (event->type() == QEvent::Expose || event->type() == QEvent::Resize) paint();
        if (event->type() == QEvent::MouseButtonPress) {
            auto mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && mouse->position().y() >= 64 && mouse->position().y() < 128) {
                const bool accepted = startSystemMove();
                std::printf("{\"event\":\"move-request\",\"accepted\":%s}\n", accepted ? "true" : "false");
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
        for (int bit = 0; bit < 32; ++bit)
            painter.fillRect(48 + bit * 17, 16, 17, 32, (id >> bit) & 1 ? Qt::white : QColor(32,40,48));
        painter.fillRect(592, 16, 16, 32, QColor(32,64,224));
        painter.fillRect(608, 16, 16, 32, QColor(224,128,32));
        // Keep the system cursor and its shadow away from the encoded bits.
        // Cursor-free requests can still contain a composited software cursor.
        painter.fillRect(0, 64, 640, 64, QColor(44, 58, 68));
        painter.setPen(Qt::white);
        painter.drawText(QRect(0,64,640,64), Qt::AlignCenter, QStringLiteral("Drag here"));
        painter.drawText(QRect(30,144,580,180), Qt::AlignCenter,
                         QStringLiteral("Vela benchmark scene\nNative Wayland • fixed 640 × 360 content"));
        painter.end();
        store.endPaint();
        store.flush(QRect(QPoint(), size()));
    }
};
int main(int argc, char **argv) {
    if (argc == 2 && QByteArray(argv[1]) == "--help") {
        std::puts("Usage: vela-bench-scene --scene | --launch | --receipt");
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
    if (operation != "--scene" && operation != "--launch") return 1;
    qputenv("QT_QPA_PLATFORM", "wayland");
    QGuiApplication app(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) return 1;
    QGuiApplication::setDesktopFileName(QStringLiteral("org.vela.BenchmarkScene"));
    if (operation == "--launch") {
        QSaveFile file(receipt);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        file.write(QJsonDocument(QJsonObject{{"schema",1},{"event","launcher-app-started"},
                   {"uid",qint64(getuid())},{"pid",qint64(getpid())},{"marker",qint64(launchedId)}}).toJson());
        if (!file.commit()) return 1;
    }
    Scene window(operation == "--launch" ? launchedId : sceneId);
    window.show();
    std::puts("{\"event\":\"scene-ready\",\"protocol\":\"wayland\",\"width\":640,\"height\":360}");
    std::fflush(stdout);
    QTimer::singleShot(180000, &app, &QCoreApplication::quit);
    return app.exec();
}
