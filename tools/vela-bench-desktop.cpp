// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Unprivileged Plasma display/capture adapter. No permission dialogs, portals
// or alternate capture routes. A missing API is an explicit failure.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <QGuiApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QImage>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QFile>
#include <QDir>
#include <KScreen/Config>
#include <KScreen/GetConfigOperation>
#include <KScreen/SetConfigOperation>
#include <KScreen/Output>
#include <KScreen/Mode>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static quint64 monotonicNs()
{
    timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) std::exit(1);
    return quint64(now.tv_sec) * 1000000000ull + now.tv_nsec;
}
static void fail(const QString &message)
{
    std::fprintf(stderr, "vela-bench-desktop: %s\n", qPrintable(message));
    std::exit(1);
}
static void print(const QJsonObject &value)
{
    const auto text = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::fwrite(text.constData(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
static void resolveWaylandSocket()
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    struct stat info;
    if (runtime.isEmpty() || lstat(QFile::encodeName(runtime).constData(), &info) || !S_ISDIR(info.st_mode)
        || info.st_uid != getuid() || (info.st_mode & 0777) != 0700) fail("invalid owned runtime directory");
    auto interface = QDBusConnection::sessionBus().interface();
    if (!interface) fail("no user bus interface");
    const auto pid = interface->servicePid(QStringLiteral("org.kde.KWin"));
    const auto uid = interface->serviceUid(QStringLiteral("org.kde.KWin"));
    if (!pid.isValid() || !uid.isValid() || !pid.value() || uid.value() != getuid()) fail("KWin bus owner is not this UID");
    QStringList matches;
    const auto names = QDir(runtime).entryList({QStringLiteral("wayland-*")}, QDir::System | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    if (names.size() > 64) fail("too many candidate Wayland endpoints");
    for (const auto &name : names) {
        const QByteArray path = QFile::encodeName(runtime + QLatin1Char('/') + name);
        if (lstat(path.constData(), &info) || !S_ISSOCK(info.st_mode) || info.st_uid != getuid()) continue;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (size_t(path.size()) >= sizeof(address.sun_path)) continue;
        std::memcpy(address.sun_path, path.constData(), path.size() + 1);
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) fail("cannot allocate endpoint validation socket");
        ucred peer{};
        socklen_t size = sizeof(peer);
        const bool verified = !connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))
            && !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) && size == sizeof(peer)
            && peer.uid == uid.value() && uint(peer.pid) == pid.value();
        close(fd);
        if (verified) matches.append(name);
    }
    const auto currentPid = interface->servicePid(QStringLiteral("org.kde.KWin"));
    if (!currentPid.isValid() || currentPid.value() != pid.value()) fail("KWin owner changed during endpoint validation");
    if (matches.size() != 1) fail("expected one live Wayland socket belonging to the KWin bus owner");
    print({{"schema", 1}, {"method", "kwin-socket-peer-v1"}, {"wayland_display", matches[0]},
           {"uid", qint64(uid.value())}, {"pid", qint64(pid.value())}});
}
static QJsonArray markers(const QImage &source)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    QJsonArray found;
    auto close = [](QRgb pixel, int r, int g, int b) {
        return std::abs(qRed(pixel) - r) <= 20 && std::abs(qGreen(pixel) - g) <= 20 && std::abs(qBlue(pixel) - b) <= 20;
    };
    for (int y = 0; y + 32 <= image.height(); ++y) {
        const auto row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x + 608 <= image.width(); ++x) {
            if (!close(row[x], 208, 32, 208) || !close(row[x + 15], 208, 32, 208) ||
                !close(row[x + 16], 32, 208, 32) || !close(row[x + 31], 32, 208, 32) ||
                !close(row[x + 576], 32, 64, 224) || !close(row[x + 592], 224, 128, 32)) continue;
            if (y && close(reinterpret_cast<const QRgb *>(image.constScanLine(y - 1))[x], 208, 32, 208)) continue;
            const auto bottom = reinterpret_cast<const QRgb *>(image.constScanLine(y + 31));
            if (!close(bottom[x], 208, 32, 208) || !close(bottom[x + 592], 224, 128, 32)) continue;
            const auto bits = reinterpret_cast<const QRgb *>(image.constScanLine(y + 16));
            quint32 frame = 0;
            bool valid = true;
            for (int bit = 0; bit < 32; ++bit) {
                const QRgb pixel = bits[x + 32 + bit * 17 + 8];
                if (qRed(pixel) > 200 && qGreen(pixel) > 200 && qBlue(pixel) > 200) frame |= quint32(1) << bit;
                else if (!close(pixel, 32, 40, 48)) { valid = false; break; }
            }
            if (valid) found.append(QJsonObject{{"x", x}, {"y", y}, {"frame", qint64(frame)}, {"marker", "vela-frame-id-v2"}});
            if (found.size() > 16) fail("ambiguous capture markers");
        }
    }
    return found;
}
static QJsonObject imageRecord(const QImage &image)
{
    if (image.isNull() || image.width() > 8192 || image.height() > 8192 || quint64(image.width()) * image.height() > 16777216) fail("invalid image dimensions");
    QByteArray png;
    QBuffer buffer(&png);
    if (!image.save(&buffer, "PNG")) fail("cannot encode capture");
    if (png.size() > 12 * 1024 * 1024) fail("capture exceeds the fixed 12 MiB PNG budget");
    QJsonArray tiles;
    for (int y = 0; y < 18; ++y)
        for (int x = 0; x < 32; ++x)
            tiles.append(int(image.pixel((2*x+1)*image.width()/64, (2*y+1)*image.height()/36) & 0xffffff));
    return {{"width", image.width()}, {"height", image.height()}, {"markers", markers(image)}, {"tiles", tiles},
            {"png_base64", QString::fromLatin1(png.toBase64())}};
}
static KScreen::ConfigPtr wait(KScreen::ConfigOperation *operation, bool setting = false)
{
    QEventLoop loop;
    KScreen::ConfigPtr result;
    QString error;
    bool failed = false;
    QObject::connect(operation, &KScreen::ConfigOperation::finished, &loop, [&](KScreen::ConfigOperation *finished) {
        failed = finished->hasError();
        error = finished->errorString();
        result = finished->config();
        loop.quit();
    });
    loop.exec();
    if (failed || !result) {
        // Validation/GetConfig failures must not authorize a greeter restart.
        // KWin's error text is translated and is not a stable error category.
        if (setting) print({{"schema", 1}, {"event", "output-configuration-failed"},
                            {"stage", "kscreen-set-config"}, {"error", error}});
        fail(error.isEmpty() ? QStringLiteral("display operation returned no configuration") : error);
    }
    return result;
}
static KScreen::ConfigPtr config()
{
    return wait(new KScreen::GetConfigOperation);
}
static QJsonObject outputs(const KScreen::ConfigPtr &config)
{
    QJsonArray rows;
    for (const auto &output : config->outputs()) {
        if (!output->isConnected()) continue;
        QJsonArray modes;
        for (const auto &mode : output->modes()) {
            modes.append(QJsonObject{{"id", mode->id()}, {"width", mode->size().width()},
                {"height", mode->size().height()}, {"refresh_mhz", qRound(mode->refreshRate() * 1000)},
                {"preferred", output->preferredModes().contains(mode->id())}});
        }
        rows.append(QJsonObject{{"name", output->name()}, {"enabled", output->isEnabled()},
            {"priority", int(output->priority())}, {"scale", output->scale()},
            {"x", output->pos().x()}, {"y", output->pos().y()}, {"rotation", int(output->rotation())},
            {"vrr", int(output->vrrPolicy())}, {"current_mode_id", output->currentModeId()}, {"modes", modes}});
    }
    return {{"schema", 1}, {"backend", "libkscreen-wayland-v1"}, {"outputs", rows}};
}
static void apply(const KScreen::ConfigPtr &config, const QByteArray &raw)
{
    if (raw.size() > 65536) fail("oversized output configuration");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) fail("expected output configuration array");
    int enabled = 0;
    int lastPriority = 0;
    for (const auto &value : document.array()) {
        const auto wanted = value.toObject();
        if (wanted["enabled"].toBool()) lastPriority = qMax(lastPriority, wanted["priority"].toInt());
    }
    for (const auto &value : document.array()) {
        const auto wanted = value.toObject();
        KScreen::OutputPtr output;
        for (const auto &candidate : config->outputs()) {
            if (candidate->name() == wanted["name"].toString() && candidate->isConnected()) output = candidate;
        }
        if (!output) fail("configured connector is not connected");
        const bool on = wanted["enabled"].toBool();
        output->setEnabled(on);
        if (!on) { output->setPriority(0); continue; }
        ++enabled;
        // KScreen mode IDs belong to one backend/configuration instance.
        // Resolve the physical mode again inside this very apply operation;
        // even a query immediately before this process can have different IDs.
        QJsonObject physicalMode;
        for (const auto &row : wanted["modes"].toArray()) {
            const auto mode = row.toObject();
            if (mode["id"].toString() == wanted["current_mode_id"].toString()) {
                if (!physicalMode.isEmpty()) fail("ambiguous configured mode");
                physicalMode = mode;
            }
        }
        if (physicalMode["width"].toInt() <= 0 || physicalMode["height"].toInt() <= 0
            || physicalMode["refresh_mhz"].toInt() <= 0) fail("invalid configured physical mode");
        QString modeId;
        for (const auto &mode : output->modes()) {
            if (mode->size() == QSize(physicalMode["width"].toInt(), physicalMode["height"].toInt())
                && qRound(mode->refreshRate() * 1000) == physicalMode["refresh_mhz"].toInt()) {
                modeId = mode->id();
                break;
            }
        }
        if (modeId.isEmpty()) fail("configured physical mode is not advertised");
        const double scale = wanted["scale"].toDouble();
        const int rotation = wanted["rotation"].toInt();
        const int vrr = wanted["vrr"].toInt(-1);
        if (!(scale >= .5 && scale <= 4) || rotation < 1 || rotation > 128 || (rotation & (rotation - 1)) || vrr < 0 || vrr > 2) fail("invalid display condition");
        output->setCurrentModeId(modeId);
        output->setScale(scale);
        output->setPos(QPoint(wanted["x"].toInt(), wanted["y"].toInt()));
        output->setRotation(KScreen::Output::Rotation(rotation));
        // A greeter can report enabled outputs with priority 0 (no known
        // ordering). KScreen treats setting 0 as disabled; retain these heads
        // after the explicitly ordered ones instead of switching them off.
        const int priority = wanted["priority"].toInt();
        output->setPriority(priority > 0 ? priority : ++lastPriority);
        output->setVrrPolicy(KScreen::Output::VrrPolicy(vrr));
    }
    if (!enabled) fail("refusing to disable every output");
    wait(new KScreen::SetConfigOperation(config), true);
}
static void capture(const QString &name, int count)
{
    for (int i = 0; i < count; ++i) {
        const quint64 begin = monotonicNs();
        int fd = memfd_create("vela-benchmark-capture", MFD_CLOEXEC);
        if (fd < 0) fail("cannot allocate capture fd");
        auto message = QDBusMessage::createMethodCall("org.kde.KWin.ScreenShot2", "/org/kde/KWin/ScreenShot2",
                                                      "org.kde.KWin.ScreenShot2", "CaptureScreen");
        message << name << QVariantMap{{"native-resolution", true}, {"include-cursor", false}, {"hide-caller-windows", false}}
                << QVariant::fromValue(QDBusUnixFileDescriptor(fd));
        QDBusReply<QVariantMap> reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 10000);
        if (!reply.isValid()) fail(reply.error().message());
        const auto metadata = reply.value();
        const quint64 width = metadata["width"].toUInt(), height = metadata["height"].toUInt();
        const quint64 stride = metadata["stride"].toUInt(), size = stride * height;
        if (metadata["type"].toString() != "raw" || !width || !height || width > 8192 || height > 8192 ||
            width * height > 16777216 || stride < width * 4 || stride > width * 16 || size > 268435456) fail("invalid capture geometry");
        struct stat info;
        while (true) {
            if (fstat(fd, &info) || quint64(info.st_size) > size) fail("invalid capture size");
            if (quint64(info.st_size) == size) break;
            if (monotonicNs() - begin > 12000000000ull) fail("capture writer deadline");
            QThread::msleep(1);
        }
        const auto end = monotonicNs();
        const auto pixels = static_cast<const uchar *>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (pixels == MAP_FAILED) fail("cannot map capture");
        QImage image(pixels, width, height, stride, QImage::Format(metadata["format"].toUInt()));
        if (image.isNull()) fail("unsupported capture format");
        auto record = imageRecord(image);
        record.insert("event", "capture"); record.insert("backend", "kwin-screenshot2-v1"); record.insert("index", i);
        record.insert("begin_monotonic_ns", QString::number(begin)); record.insert("end_monotonic_ns", QString::number(end));
        print(record);
        munmap(const_cast<uchar *>(pixels), size);
        close(fd);
        if (i + 1 < count) QThread::msleep(100);
    }
}
int main(int argc, char **argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--help")) {
        std::puts("Usage: vela-bench-desktop --resolve-wayland | --outputs | --apply JSON | --capture CONNECTOR [COUNT] | --analyze PNG\nUnprivileged adapter; JSONL output; no backend fallback.");
        return 0;
    }
    if (getuid() == 0 || geteuid() == 0) fail("desktop operations must run unprivileged");
    signal(SIGALRM, [](int) { _exit(124); });
    alarm(60);
    if (argc == 2 && !std::strcmp(argv[1], "--resolve-wayland")) {
        QCoreApplication application(argc, argv);
        resolveWaylandSocket();
        return 0;
    }
    if (argc == 3 && !std::strcmp(argv[1], "--analyze")) {
        QCoreApplication application(argc, argv);
        QFile input(QString::fromLocal8Bit(argv[2]));
        if (!input.open(QIODevice::ReadOnly) || input.size() > 67108864) fail("invalid capture file");
        const QByteArray header = input.peek(24);
        if (header.size() != 24 || header.left(8) != QByteArray::fromHex("89504e470d0a1a0a")) fail("expected PNG");
        auto number = [&](int offset) { quint32 result = 0; for (int i = 0; i < 4; ++i) result = (result << 8) | quint8(header[offset + i]); return result; };
        if (!number(16) || !number(20) || number(16) > 8192 || number(20) > 8192 || quint64(number(16)) * number(20) > 16777216) fail("oversized PNG");
        print(imageRecord(QImage::fromData(input.readAll(), "PNG")));
        return 0;
    }
    QGuiApplication application(argc, argv);
    if (argc == 2 && !std::strcmp(argv[1], "--outputs")) print(outputs(config()));
    else if (argc == 3 && !std::strcmp(argv[1], "--apply")) { apply(config(), argv[2]); print(outputs(config())); }
    else if ((argc == 3 || argc == 4) && !std::strcmp(argv[1], "--capture")) {
        bool valid = true;
        int count = argc == 4 ? QString::fromLocal8Bit(argv[3]).toInt(&valid) : 1;
        if (!valid || count < 1 || count > 12) fail("capture count outside 1..12");
        capture(QString::fromLocal8Bit(argv[2]), count);
    } else fail("unsupported operation; see --help");
}
