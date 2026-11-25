// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard.h"

#include "shellcontroller.h"
#include "../../common/privatefile.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QVariantMap>

#include <algorithm>
#include <cstring>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

namespace {

constexpr int maxItems = 25; // like Windows
constexpr qsizetype maxText = 1024 * 1024;
constexpr qsizetype maxImage = 32 * 1024 * 1024;

const char* const textTypes[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT" };

wl_display* waylandDisplay()
{
    auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    return wayland ? wayland->display() : nullptr;
}

QString storageDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/vela/clipboard");
}

} // namespace

// A clipboard offer and its formats (lives in the clipboard thread).
struct Clipboard::Offer {
    ext_data_control_offer_v1* offer = nullptr;
    QStringList types;
};

struct ClipboardCallbacks {
    using Offer = Clipboard::Offer;

    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t)
    {
        auto* self = static_cast<Clipboard*>(data);
        if (!std::strcmp(interface, ext_data_control_manager_v1_interface.name)) {
            self->m_manager = static_cast<ext_data_control_manager_v1*>(
                wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
            auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
            if (wayland && wayland->seat()) {
                self->m_device = ext_data_control_manager_v1_get_data_device(self->m_manager, wayland->seat());
                ext_data_control_device_v1_add_listener(self->m_device, &deviceListener, self);
            }
        }
    }
    static void globalRemove(void*, wl_registry*, uint32_t) { }
    static constexpr wl_registry_listener registryListener { global, globalRemove };

    // --- device ---
    static void dataOffer(void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* id)
    {
        auto* self = static_cast<Clipboard*>(data);
        auto* offer = new Offer;
        offer->offer = id;
        self->m_offers.insert(id, offer);
        ext_data_control_offer_v1_add_listener(id, &offerListener, offer);
    }
    static void selection(void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* id)
    {
        auto* self = static_cast<Clipboard*>(data);
        Offer* offer = id ? self->m_offers.value(id) : nullptr;
        // Old offers are no longer needed.
        for (auto it = self->m_offers.begin(); it != self->m_offers.end();) {
            if (it.value() != offer) {
                ext_data_control_offer_v1_destroy(it.value()->offer);
                delete it.value();
                it = self->m_offers.erase(it);
            } else {
                ++it;
            }
        }
        if (!offer) {
            return;
        }
        if (self->m_ignoreNext.exchange(false)) {
            return; // we set it ourselves
        }
        self->readOffer(offer);
        self->m_offers.remove(offer->offer);
        ext_data_control_offer_v1_destroy(offer->offer);
        delete offer;
    }
    static void finished(void* data, ext_data_control_device_v1* device)
    {
        auto* self = static_cast<Clipboard*>(data);
        ext_data_control_device_v1_destroy(device);
        self->m_device = nullptr;
    }
    static void primarySelection(void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* id)
    {
        // The primary selection (highlighted text) doesn't go into the
        // history.
        auto* self = static_cast<Clipboard*>(data);
        if (Offer* offer = id ? self->m_offers.take(id) : nullptr) {
            ext_data_control_offer_v1_destroy(offer->offer);
            delete offer;
        }
    }
    static constexpr ext_data_control_device_v1_listener deviceListener {
        dataOffer, selection, finished, primarySelection
    };

    // --- offer ---
    static void offerType(void* data, ext_data_control_offer_v1*, const char* mimeType)
    {
        static_cast<Offer*>(data)->types << QString::fromUtf8(mimeType);
    }
    static constexpr ext_data_control_offer_v1_listener offerListener { offerType };

    // --- our source ---
    static void send(void* data, ext_data_control_source_v1*, const char* mimeType, int32_t fd)
    {
        auto* self = static_cast<Clipboard*>(data);
        QByteArray payload;
        {
            QMutexLocker lock(&self->m_mutex);
            payload = QByteArray(mimeType) == "image/png" ? self->m_sourcePng : self->m_sourceText;
        }
        // In another thread: a large image doesn't stall the shell while the
        // app reads it.
        std::thread([fd, payload] {
            qsizetype written = 0;
            while (written < payload.size()) {
                const ssize_t n = ::write(fd, payload.constData() + written, size_t(payload.size() - written));
                if (n <= 0) {
                    break;
                }
                written += n;
            }
            ::close(fd);
        }).detach();
    }
    static void cancelled(void*, ext_data_control_source_v1* source)
    {
        ext_data_control_source_v1_destroy(source); // another app copied something
    }
    static constexpr ext_data_control_source_v1_listener sourceListener { send, cancelled };
};

// ------------------------------------------------------------- Clipboard --

Clipboard::Clipboard(QObject* parent)
    : QObject(parent)
{
    m_enabled = QSettings().value(QStringLiteral("clipboard/history"), false).toBool();
    loadPinned();
    wl_display* display = waylandDisplay();
    if (!display || pipe2(m_wake, O_CLOEXEC | O_NONBLOCK) != 0) {
        return;
    }
    // A queue of our own: the clipboard thread serves its events.
    m_queue = wl_display_create_queue(display);
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), m_queue);
    m_registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(m_registry, &ClipboardCallbacks::registryListener, this);
    wl_display_flush(display);
    m_thread = QThread::create([this] { run(); });
    m_thread->start();
}

Clipboard::~Clipboard()
{
    if (m_thread) {
        m_stop = true;
        (void)!::write(m_wake[1], "x", 1);
        m_thread->wait();
        delete m_thread;
    }
    for (Offer* offer : std::as_const(m_offers)) {
        delete offer;
    }
    for (int fd : m_wake) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
}

void Clipboard::run()
{
    // The correct way to read the same connection from several threads:
    // prepare_read, poll, read_events, then our queue's events.
    wl_display* display = waylandDisplay();
    while (!m_stop) {
        while (wl_display_prepare_read_queue(display, m_queue) != 0) {
            wl_display_dispatch_queue_pending(display, m_queue);
        }
        wl_display_flush(display);
        pollfd fds[2] = { { wl_display_get_fd(display), POLLIN, 0 }, { m_wake[0], POLLIN, 0 } };
        if (poll(fds, 2, -1) > 0 && (fds[0].revents & POLLIN)) {
            wl_display_read_events(display);
        } else {
            wl_display_cancel_read(display);
        }
        wl_display_dispatch_queue_pending(display, m_queue);
    }
}

void Clipboard::flush()
{
    if (wl_display* display = waylandDisplay()) {
        wl_display_flush(display);
    }
}

void Clipboard::setEnabled(bool on)
{
    if (on == m_enabled) {
        return;
    }
    m_enabled = on;
    QSettings settings;
    settings.setValue(QStringLiteral("clipboard/history"), on);
    if (!on) {
        clear();
    }
    emit enabledChanged();
}

QVariantList Clipboard::items() const
{
    QVariantList out;
    // Pinned first, then the others; each most recent first.
    for (const bool pinned : { true, false }) {
        for (const Item& item : m_items) {
            if (item.pinned != pinned) {
                continue;
            }
            out.append(QVariantMap {
                { QStringLiteral("id"), item.id },
                { QStringLiteral("kind"), item.png.isEmpty() ? QStringLiteral("text") : QStringLiteral("image") },
                { QStringLiteral("text"), item.text.left(2000) },
                { QStringLiteral("pinned"), item.pinned },
                { QStringLiteral("time"), item.time },
            });
        }
    }
    return out;
}

bool Clipboard::Item::setImage(const QImage& image, QByteArray originalPng)
{
    if (image.isNull()) return false;
    if (originalPng.isEmpty()) {
        QBuffer buffer(&originalPng);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return false;
    }
    png = std::move(originalPng);
    originalSize = image.size();
    // The 272 × 96 logical-pixel card at 2×. Larger requests use the PNG.
    const QSize bounds(544, 192);
    preview = originalSize.boundedTo(bounds) == originalSize
        ? image
        : image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return true;
}

bool Clipboard::Item::sameImage(const Item& other) const
{
    if (png.isEmpty() || originalSize != other.originalSize) return false;
    if (png == other.png) return true;
    if (preview != other.preview) return false;
    // Different encodings can represent the same pixels. A thumbnail match
    // alone would merge distinct screenshots.
    const QImage full = QImage::fromData(png, "PNG");
    return !full.isNull() && full == QImage::fromData(other.png, "PNG");
}

QImage Clipboard::image(int id, const QSize& requested) const
{
    Item snapshot;
    {
        QMutexLocker lock(&m_itemsMutex);
        for (const Item& item : m_items) {
            if (item.id == id) {
                snapshot = item;
                break;
            }
        }
    }
    if (snapshot.png.isEmpty()) return {};
    const QSize target = requested.isEmpty()
        ? snapshot.originalSize
        : snapshot.originalSize.scaled(requested, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    QImage image = snapshot.preview;
    if (image.size() != snapshot.originalSize && (target.width() > image.width() || target.height() > image.height())) {
        image = QImage::fromData(snapshot.png, "PNG");
    }
    return image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void Clipboard::readOffer(Offer* offer)
{
    // In the clipboard thread: read to the end, then the item goes to the main
    // thread.
    const QStringList& types = offer->types;
    // Password managers ask not to be remembered; copied files (Explorer,
    // Dolphin) aren't "content" and stay on the normal clipboard.
    if (types.contains(QStringLiteral("x-kde-passwordManagerHint")) || types.contains(QStringLiteral("text/uri-list"))
        || types.contains(QStringLiteral("x-special/gnome-copied-files")) || !m_enabled) {
        return;
    }
    QString chosen;
    for (const char* type : textTypes) {
        if (types.contains(QLatin1String(type))) {
            chosen = QLatin1String(type);
            break;
        }
    }
    if (chosen.isEmpty()) {
        for (const char* type : { "image/png", "image/jpeg", "image/webp", "image/bmp" }) {
            if (types.contains(QLatin1String(type))) {
                chosen = QLatin1String(type);
                break;
            }
        }
    }
    if (chosen.isEmpty()) {
        return;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return;
    }
    ext_data_control_offer_v1_receive(offer->offer, chosen.toUtf8().constData(), fds[1]);
    wl_display_flush(waylandDisplay());
    ::close(fds[1]);
    QByteArray data;
    char chunk[65536];
    for (;;) {
        pollfd p { fds[0], POLLIN, 0 };
        if (poll(&p, 1, 2000) <= 0) {
            break; // the app doesn't answer: give up
        }
        const ssize_t n = ::read(fds[0], chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        data.append(chunk, n);
        if (data.size() > maxImage) {
            ::close(fds[0]);
            return;
        }
    }
    ::close(fds[0]);

    Item item;
    if (chosen.startsWith(QLatin1String("image/"))) {
        const QImage image = QImage::fromData(data);
        if (!item.setImage(image, chosen == QLatin1String("image/png") ? data : QByteArray())) return;
    } else {
        if (data.size() > maxText) {
            return;
        }
        item.text = QString::fromUtf8(data);
        if (item.text.trimmed().isEmpty()) {
            return;
        }
    }
    QMetaObject::invokeMethod(this, [this, item] { add(item); }, Qt::QueuedConnection);
}

void Clipboard::add(Item item)
{
    if (!m_enabled) {
        return;
    }
    QMutexLocker lock(&m_itemsMutex);
    item.time = QDateTime::currentDateTime();
    // Already in the list: back to the top (pinned if it was).
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        const Item& old = m_items[i];
        if ((item.id && item.id == old.id) || (!item.text.isEmpty() && old.text == item.text) || item.sameImage(old)) {
            item.pinned = old.pinned;
            item.id = old.id;
            m_items.removeAt(i);
            break;
        }
    }
    if (item.id == 0) {
        item.id = m_nextId++;
    }
    m_items.prepend(item);
    // Past the limit the oldest unpinned items are lost.
    int unpinned = 0;
    for (qsizetype i = 0; i < m_items.size();) {
        if (!m_items[i].pinned && ++unpinned > maxItems) {
            m_items.removeAt(i);
        } else {
            ++i;
        }
    }
    lock.unlock();
    emit itemsChanged();
}

void Clipboard::setSelection(const Item& item)
{
    if (!m_manager || !m_device) {
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_sourceText = item.png.isEmpty() ? item.text.toUtf8() : QByteArray();
        m_sourcePng = item.png;
    }
    auto* source = ext_data_control_manager_v1_create_data_source(m_manager);
    ext_data_control_source_v1_add_listener(source, &ClipboardCallbacks::sourceListener, this);
    if (item.png.isEmpty()) {
        for (const char* type : textTypes) {
            ext_data_control_source_v1_offer(source, type);
        }
    } else {
        ext_data_control_source_v1_offer(source, "image/png");
    }
    m_ignoreNext = true;
    ext_data_control_device_v1_set_selection(m_device, source);
    flush();
}

void Clipboard::paste(int id)
{
    for (const Item& item : std::as_const(m_items)) {
        if (item.id == id) {
            setSelection(item);
            // Like Windows: back to the top.
            Item copy = item;
            add(copy);
            ShellController::sendToCompositor("paste");
            return;
        }
    }
}

void Clipboard::remove(int id)
{
    QMutexLocker lock(&m_itemsMutex);
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id) {
            const bool pinned = m_items[i].pinned;
            m_items.removeAt(i);
            lock.unlock();
            if (pinned) {
                savePinned();
            }
            emit itemsChanged();
            return;
        }
    }
}

void Clipboard::togglePin(int id)
{
    QMutexLocker lock(&m_itemsMutex);
    for (Item& item : m_items) {
        if (item.id == id) {
            item.pinned = !item.pinned;
            lock.unlock();
            savePinned();
            emit itemsChanged();
            return;
        }
    }
}

void Clipboard::clear()
{
    QMutexLocker lock(&m_itemsMutex);
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [](const Item& item) { return !item.pinned; }),
        m_items.end());
    lock.unlock();
    emit itemsChanged();
}

void Clipboard::copyText(const QString& text)
{
    Item item;
    item.text = text;
    setSelection(item);
    add(item);
}

void Clipboard::copyImage(const QImage& image)
{
    Item item;
    if (!item.setImage(image)) return;
    setSelection(item);
    add(item);
}

// -------------------------------------------------------- pinned items --

void Clipboard::savePinned() const
{
    const QDir dir(storageDir());
    if (!dir.mkpath(QStringLiteral("."))) return;
    QSet<QString> liveFiles;
    QJsonArray list;
    for (const Item& item : m_items) {
        if (!item.pinned) continue;
        if (item.png.isEmpty()) {
            list.append(QJsonObject { { QStringLiteral("text"), item.text } });
            continue;
        }
        const QString name = QString::fromLatin1(QCryptographicHash::hash(item.png, QCryptographicHash::Sha256).toHex())
            + QStringLiteral(".png");
        if (!dir.exists(name)) {
            QSaveFile file(dir.filePath(name));
            if (!vela::openPrivateFile(file) || file.write(item.png) != item.png.size() || !file.commit()) return;
        }
        liveFiles.insert(name);
        list.append(QJsonObject { { QStringLiteral("image"), name } });
    }
    QSaveFile file(dir.filePath(QStringLiteral("pinned.json")));
    const QByteArray json = QJsonDocument(list).toJson(QJsonDocument::Compact);
    if (!vela::openPrivateFile(file) || file.write(json) != json.size() || !file.commit()) return;
    for (const QString& old : dir.entryList({ QStringLiteral("*.png") }, QDir::Files)) {
        if (!liveFiles.contains(old)) QFile::remove(dir.filePath(old));
    }
}

void Clipboard::loadPinned()
{
    const QDir dir(storageDir());
    QFile file(dir.filePath(QStringLiteral("pinned.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    for (const QJsonValue& value : QJsonDocument::fromJson(file.readAll()).array()) {
        const QJsonObject object = value.toObject();
        Item item;
        item.id = m_nextId++;
        item.pinned = true;
        item.time = QDateTime::currentDateTime();
        if (object.contains(QStringLiteral("image"))) {
            QFile png(dir.filePath(object[QStringLiteral("image")].toString()));
            if (!png.open(QIODevice::ReadOnly)) continue;
            const QByteArray bytes = png.readAll();
            if (!item.setImage(QImage::fromData(bytes, "PNG"), bytes)) continue;
        } else {
            item.text = object[QStringLiteral("text")].toString();
        }
        m_items.append(item);
    }
}
