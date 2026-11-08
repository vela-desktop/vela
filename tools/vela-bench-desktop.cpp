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
#include <QDBusArgument>
#include <QDBusVariant>
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
#include <QSet>
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
        for (int x = 0; x + 320 <= image.width(); ++x) {
            int stride = 0;
            for (int step : {17, 8})
                if (x + 64 + 32 * step <= image.width() && close(row[x + 32 + 32 * step],32,64,224)
                    && close(row[x + 48 + 32 * step],224,128,32)) { stride = step; break; }
            if (!stride) continue;
            const int tail = x + 48 + 32 * stride;
            if (!close(row[x], 208, 32, 208) || !close(row[x + 15], 208, 32, 208) ||
                !close(row[x + 16], 32, 208, 32) || !close(row[x + 31], 32, 208, 32)) continue;
            if (y && close(reinterpret_cast<const QRgb *>(image.constScanLine(y - 1))[x], 208, 32, 208)) continue;
            const auto bottom = reinterpret_cast<const QRgb *>(image.constScanLine(y + 31));
            if (!close(bottom[x], 208, 32, 208) || !close(bottom[tail], 224, 128, 32)) continue;
            const auto bits = reinterpret_cast<const QRgb *>(image.constScanLine(y + 16));
            quint32 frame = 0;
            bool valid = true;
            for (int bit = 0; bit < 32; ++bit) {
                const QRgb pixel = bits[x + 32 + bit * stride + stride / 2];
                if (qRed(pixel) > 200 && qGreen(pixel) > 200 && qBlue(pixel) > 200) frame |= quint32(1) << bit;
                else if (!close(pixel, 32, 40, 48)) { valid = false; break; }
            }
            if (valid) found.append(QJsonObject{{"x", x}, {"y", y}, {"frame", qint64(frame)}, {"marker", "vela-frame-id-v2"}});
            if (found.size() > 16) fail("ambiguous capture markers");
        }
    }
    return found;
}
static QJsonObject sceneRectangles(const QImage &image,const QJsonArray &found)
{
    QJsonObject result;
    for(const auto value:found) {
        const auto m=value.toObject();const auto id=m["frame"].toInteger();
        if(id!=0x715000 && id!=0x715002)continue;
        const QRgb color=id==0x715000?qRgb(200,80,120):qRgb(200,120,80);
        int matches=0,cx=0,cy=0;
        for(int y=0;y+16<=image.height();++y)for(int x=0;x+16<=image.width();++x) {
            if((image.pixel(x,y)|0xff000000)!=color || (x && (image.pixel(x-1,y)|0xff000000)==color)
               || (y && (image.pixel(x,y-1)|0xff000000)==color))continue;
            if((image.pixel(x+15,y)|0xff000000)!=color || (image.pixel(x,y+15)|0xff000000)!=color
               || (image.pixel(x+15,y+15)|0xff000000)!=color || (image.pixel(x+8,y+8)|0xff000000)!=color)continue;
            ++matches;cx=x+8;cy=y+8;
        }
        if(matches==1) {int x=m["x"].toInt()-16,y=m["y"].toInt()-16;result[QString::number(id)]=QJsonArray{x,y,cx-x+16,cy-y+16};}
    }
    return result;
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
    const auto found=markers(image);
    return {{"width", image.width()}, {"height", image.height()}, {"markers", found}, {"tiles", tiles},
            {"rectangles",sceneRectangles(image,found)},
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
    QSet<QString> connectors;
    for (const auto &value : document.array()) {
        const auto wanted = value.toObject();
        const QString name = wanted["name"].toString();
        if (name.isEmpty() || connectors.contains(name)) fail("duplicate or empty configured connector");
        connectors.insert(name);
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
static QByteArray velaCommand(const QByteArray &command,bool answer)
{
    const QByteArray runtime=qgetenv("XDG_RUNTIME_DIR"),display=qgetenv("WAYLAND_DISPLAY");
    if(runtime.isEmpty() || display.isEmpty() || display.contains('/') || display.contains(".."))fail("invalid Vela workspace endpoint");
    QByteArray path=runtime+"/vela-"+display+".sock";
    sockaddr_un address{};address.sun_family=AF_UNIX;
    if(size_t(path.size())>=sizeof(address.sun_path))fail("workspace endpoint too long");
    std::memcpy(address.sun_path,path.constData(),path.size()+1);
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    timeval timeout{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    ucred peer{};socklen_t length=sizeof peer;
    if(fd<0 || connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof address)
       || getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length) || peer.uid!=getuid())fail("workspace socket is not owned by this UID");
    if(write(fd,command.constData(),size_t(command.size()))!=command.size())fail("workspace command write");
    QByteArray reply;
    if(answer)while(!reply.endsWith('\n')) {char buffer[4096];ssize_t n=read(fd,buffer,sizeof buffer);if(n<=0 || reply.size()>262144)fail("workspace state unavailable");reply.append(buffer,n);}
    close(fd);return reply;
}
static QJsonObject workspaceState(const QString &target)
{
    QJsonArray names,ids;QString current;
    if(target=="vela") {
        auto value=QJsonDocument::fromJson(velaCommand("workspaces\n",true)).object();
        names=value["names"].toArray();int index=value["current"].toInt(-1);
        if(index<0 || index>=names.size())fail("invalid Vela workspace state");
        for(int i=0;i<names.size();++i)ids.append(QString::number(i));
        current=QString::number(index);
    } else if(target=="plasma") {
        auto message=QDBusMessage::createMethodCall("org.kde.KWin","/VirtualDesktopManager","org.freedesktop.DBus.Properties","GetAll");
        message<<QString("org.kde.KWin.VirtualDesktopManager");
        const auto reply=QDBusConnection::sessionBus().call(message,QDBus::Block,3000);
        if(reply.type()==QDBusMessage::ErrorMessage || reply.arguments().size()!=1)fail("KWin workspace properties unavailable");
        const QVariantMap map=qdbus_cast<QVariantMap>(reply.arguments()[0]);
        current=map.value("current").toString();
        const QDBusArgument desks=qvariant_cast<QDBusArgument>(map.value("desktops"));
        desks.beginArray();while(!desks.atEnd()) {int position;QString id,name;desks.beginStructure();desks>>position>>id>>name;desks.endStructure();ids.append(id);names.append(name);}desks.endArray();
        if(names.size()!=map.value("count").toInt())fail("KWin workspace count differs");
    } else fail("unknown workspace adapter");
    int index=-1;for(int i=0;i<ids.size();++i)if(ids[i].toString()==current)index=i;
    if(names.isEmpty() || names.size()>32 || index<0)fail("invalid workspace topology");
    return {{"schema",1},{"event","workspace-state"},{"target",target},{"uid",qint64(getuid())},
        {"count",names.size()},{"current_index",index},{"ids",ids},{"names",names}};
}
static void workspace(const QString &target,const QString &operation)
{
    auto initial=workspaceState(target);const int count=initial["count"].toInt();
    if(operation=="create" && count==1) {
        if(target=="vela") {
            velaCommand("workspace new\n",false);
            bool created=false;
            for(int attempt=0;attempt<60;++attempt) {
                if(workspaceState(target)["count"].toInt()==2) {created=true;break;}
                QThread::msleep(50);
            }
            if(!created)fail("workspace creation was not acknowledged");
            velaCommand("workspace rename 1 Vela benchmark fixture\n",false);
        }
        else {
            auto request=QDBusMessage::createMethodCall("org.kde.KWin","/VirtualDesktopManager","org.kde.KWin.VirtualDesktopManager","createDesktop");
            request<<quint32(1)<<QString("Vela benchmark fixture");
            if(QDBusConnection::sessionBus().call(request,QDBus::Block,3000).type()==QDBusMessage::ErrorMessage)fail("workspace fixture creation failed");
        }
    } else if(operation=="remove") {
        if(count!=2 || initial["names"].toArray()[1].toString()!="Vela benchmark fixture" || initial["current_index"].toInt()!=0)
            fail("refusing to remove a workspace not created by the fixture");
        if(target=="vela")velaCommand("workspace close 1\n",false);
        else {
            auto request=QDBusMessage::createMethodCall("org.kde.KWin","/VirtualDesktopManager","org.kde.KWin.VirtualDesktopManager","removeDesktop");
            request<<initial["ids"].toArray()[1].toString();
            if(QDBusConnection::sessionBus().call(request,QDBus::Block,3000).type()==QDBusMessage::ErrorMessage)fail("workspace fixture removal failed");
        }
    } else if(operation!="state" && operation!="create")fail("unknown workspace operation");
    QJsonObject settled;
    for(int attempt=0;attempt<30;++attempt) {
        settled=workspaceState(target);
        if((operation=="create" && count==1 && (settled["count"].toInt()!=2
                || settled["names"].toArray()[1].toString()!="Vela benchmark fixture"))
           || (operation=="remove" && settled["count"].toInt()!=1)) {QThread::msleep(50);continue;}
        settled["begin_monotonic_ns"]=QString::number(monotonicNs());print(settled);return;
    }
    fail("workspace fixture state did not settle");
}
int main(int argc, char **argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--help")) {
        std::puts("Usage: vela-bench-desktop --resolve-wayland | --outputs | --apply JSON | --capture CONNECTOR [COUNT] | --analyze PNG | --workspace TARGET state|create|remove\nUnprivileged adapter; JSONL output; no backend fallback.");
        return 0;
    }
    if (getuid() == 0 || geteuid() == 0) fail("desktop operations must run unprivileged");
    signal(SIGALRM, [](int) { _exit(124); });
    alarm(60);
    if(argc==4 && !std::strcmp(argv[1],"--workspace")) {
        QCoreApplication application(argc,argv);workspace(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]));return 0;
    }
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
