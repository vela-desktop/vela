// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowcapture.h"

#include <QGuiApplication>
#include <QScreen>
#include <QVariantMap>
#include <QtGui/qguiapplication_platform.h>
#include <QtGui/qscreen_platform.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include "vela/capture.h"

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

// ------------------------------------------------------------------ state --

struct WindowCapture::Window {
    ext_foreign_toplevel_handle_v1* handle = nullptr;
    QString identifier;
    QString title;
    QString appId;
};

// UI-side bookkeeping; shared C capture owns the Wayland session and SHM buffer.
struct WindowCapture::Job {
    WindowCapture* owner = nullptr;
    QString identifier;
    vela_capture_job* capture = nullptr;
    bool full = false; // the whole image, not the thumbnail
};

namespace {

// wl_shm formats we can read, and the matching QImage.
QImage::Format imageFormat(uint32_t shmFormat)
{
    switch (shmFormat) {
    case WL_SHM_FORMAT_XRGB8888: return QImage::Format_RGB32;
    case WL_SHM_FORMAT_ARGB8888: return QImage::Format_ARGB32_Premultiplied;
    case WL_SHM_FORMAT_XBGR8888: return QImage::Format_RGBX8888;
    case WL_SHM_FORMAT_ABGR8888: return QImage::Format_RGBA8888_Premultiplied;
    default: return QImage::Format_Invalid;
    }
}

/** Copy an output capture upright, validating metadata once per frame.
 * Non-upright transforms use precomputed signed source strides instead of
 * revalidating and switching on the transform for every pixel.
 */
QImage uprightImage(const vela_capture_frame* frame)
{
    uint32_t width = 0, height = 0;
    if (!vela_capture_frame_dimensions(frame, &width, &height)
        || width > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || height > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || frame->stride > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const QImage::Format format = imageFormat(frame->format);
    if (frame->transform == WL_OUTPUT_TRANSFORM_NORMAL) {
        return QImage(frame->pixels, int(width), int(height), int(frame->stride), format).copy();
    }
    QImage image(int(width), int(height), format);
    if (image.isNull()) return {};

    const ptrdiff_t stride = static_cast<ptrdiff_t>(frame->stride);
    const uchar* first = frame->pixels;
    ptrdiff_t xStep = 4, yStep = stride;
    switch (frame->transform) {
    case WL_OUTPUT_TRANSFORM_90:
        first += size_t(frame->height - 1) * frame->stride;
        xStep = -stride; yStep = 4;
        break;
    case WL_OUTPUT_TRANSFORM_180:
        first += size_t(frame->height - 1) * frame->stride + size_t(frame->width - 1) * 4;
        xStep = -4; yStep = -stride;
        break;
    case WL_OUTPUT_TRANSFORM_270:
        first += size_t(frame->width - 1) * 4;
        xStep = stride; yStep = -4;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
        first += size_t(frame->width - 1) * 4;
        xStep = -4;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        xStep = stride; yStep = 4;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        first += size_t(frame->height - 1) * frame->stride;
        yStep = -stride;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        first += size_t(frame->height - 1) * frame->stride + size_t(frame->width - 1) * 4;
        xStep = -stride; yStep = -4;
        break;
    default:
        return {};
    }

    for (uint32_t y = 0; y < height; ++y) {
        uchar* dst = image.scanLine(int(y));
        const uchar* src = first;
        if (xStep == 4) {
            memcpy(dst, src, size_t(width) * 4);
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                memcpy(dst + size_t(x) * 4, src, 4);
                if (x + 1 < width) src += xStep;
            }
        }
        if (y + 1 < height) first += yStep;
    }
    return image;
}

} // namespace

// libwayland's C listeners: they forward to the class.
struct CaptureCallbacks {
    using Window = WindowCapture::Window;
    using Job = WindowCapture::Job;

    // --- registry ---
    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t)
    {
        auto* self = static_cast<WindowCapture*>(data);
        if (!strcmp(interface, wl_shm_interface.name)) {
            self->m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (!strcmp(interface, ext_foreign_toplevel_list_v1_interface.name)) {
            self->m_list = static_cast<ext_foreign_toplevel_list_v1*>(
                wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1));
            ext_foreign_toplevel_list_v1_add_listener(self->m_list, &listListener, self);
        } else if (!strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name)) {
            self->m_sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
                wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
        } else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
            self->m_outputSources = static_cast<ext_output_image_capture_source_manager_v1*>(
                wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
        } else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
            self->m_copier = static_cast<ext_image_copy_capture_manager_v1*>(
                wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
        }
    }
    static void globalRemove(void*, wl_registry*, uint32_t) { }
    static constexpr wl_registry_listener registryListener { global, globalRemove };

    // --- window list ---
    static void toplevel(void* data, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle)
    {
        auto* self = static_cast<WindowCapture*>(data);
        auto* window = new Window;
        window->handle = handle;
        self->m_windows.insert(handle, window);
        ext_foreign_toplevel_handle_v1_add_listener(handle, &handleListener, self);
    }
    static void finished(void*, ext_foreign_toplevel_list_v1*) { }
    static constexpr ext_foreign_toplevel_list_v1_listener listListener { toplevel, finished };

    static void closed(void* data, ext_foreign_toplevel_handle_v1* handle)
    {
        auto* self = static_cast<WindowCapture*>(data);
        if (Window* window = self->m_windows.take(handle)) {
            self->m_thumbnails.remove(window->identifier);
            delete window;
        }
        ext_foreign_toplevel_handle_v1_destroy(handle);
    }
    static void done(void*, ext_foreign_toplevel_handle_v1*) { }
    static void title(void* data, ext_foreign_toplevel_handle_v1* handle, const char* value)
    {
        if (Window* w = static_cast<WindowCapture*>(data)->m_windows.value(handle)) {
            w->title = QString::fromUtf8(value);
        }
    }
    static void appId(void* data, ext_foreign_toplevel_handle_v1* handle, const char* value)
    {
        if (Window* w = static_cast<WindowCapture*>(data)->m_windows.value(handle)) {
            w->appId = QString::fromUtf8(value);
        }
    }
    static void identifier(void* data, ext_foreign_toplevel_handle_v1* handle, const char* value)
    {
        if (Window* w = static_cast<WindowCapture*>(data)->m_windows.value(handle)) {
            w->identifier = QString::fromUtf8(value);
        }
    }
    static constexpr ext_foreign_toplevel_handle_v1_listener handleListener {
        closed, done, title, appId, identifier
    };

    /** Forward capture completion to Qt; finishJob may destroy the capture job. */
    static void captured(vela_capture_job*, bool ok, void* userdata)
    {
        auto* job = static_cast<Job*>(userdata);
        job->owner->finishJob(job, ok);
    }
};

// ---------------------------------------------------------- WindowCapture --

WindowCapture::WindowCapture(QObject* parent)
    : QObject(parent)
{
    auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland || !wayland->display()) {
        return;
    }
    m_registry = wl_display_get_registry(wayland->display());
    wl_registry_add_listener(m_registry, &CaptureCallbacks::registryListener, this);
    wl_display_flush(wayland->display());
}

WindowCapture::~WindowCapture()
{
    const QList<Job*> jobs = m_jobs; // finishJob removes them from the list
    for (Job* job : jobs) {
        finishJob(job, false);
    }
    qDeleteAll(m_windows);
}

QString WindowCapture::title(const QString& identifier) const
{
    for (const Window* window : m_windows) {
        if (window->identifier == identifier) {
            return window->title;
        }
    }
    return {};
}

QString WindowCapture::appId(const QString& identifier) const
{
    for (const Window* window : m_windows) {
        if (window->identifier == identifier) {
            return window->appId;
        }
    }
    return {};
}

/** Start asynchronous image captures for matching Alt-Tab window identifiers. */
void WindowCapture::capture(const QStringList& identifiers)
{
    if (!m_shm || !m_sources || !m_copier) {
        qWarning("vela-shell: the compositor doesn't offer window capture");
        return;
    }
    for (const Window* window : std::as_const(m_windows)) {
        if (!identifiers.contains(window->identifier)) {
            continue;
        }
        auto* job = new Job;
        job->owner = this;
        job->identifier = window->identifier;
        auto* source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(m_sources, window->handle);
        job->capture = vela_capture_begin(m_shm, m_copier, source, &CaptureCallbacks::captured, job);
        if (!job->capture) {
            if (source) ext_image_capture_source_v1_destroy(source);
            delete job;
            continue;
        }
        m_jobs.append(job);
    }
    if (auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
        wl_display_flush(wayland->display());
    }
}

QVariantList WindowCapture::windowList() const
{
    QVariantList out;
    for (const Window* window : m_windows) {
        if (!window->identifier.isEmpty()) {
            out.append(QVariantMap { { QStringLiteral("id"), window->identifier },
                { QStringLiteral("title"), window->title }, { QStringLiteral("appId"), window->appId } });
        }
    }
    return out;
}

/** Queue downscaled previews of every Qt-visible Wayland output. */
void WindowCapture::captureScreens()
{
    if (!m_shm || !m_outputSources || !m_copier) {
        qWarning("vela-shell: the compositor doesn't offer output capture");
        return;
    }
    for (QScreen* screen : QGuiApplication::screens()) {
        auto* native = screen->nativeInterface<QNativeInterface::QWaylandScreen>();
        if (!native || !native->output()) {
            continue;
        }
        auto* job = new Job;
        job->owner = this;
        job->identifier = QStringLiteral("screen:") + screen->name();
        auto* source = ext_output_image_capture_source_manager_v1_create_source(m_outputSources, native->output());
        job->capture = vela_capture_begin(m_shm, m_copier, source, &CaptureCallbacks::captured, job);
        if (!job->capture) {
            if (source) ext_image_capture_source_v1_destroy(source);
            delete job;
            continue;
        }
        m_jobs.append(job);
    }
    if (auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
        wl_display_flush(wayland->display());
    }
}

/** Clear snapshots and cancel outstanding full-screen captures without signals. */
void WindowCapture::clearFull()
{
    // A cancelled/timed-out snip must not complete into a subsequent one.
    const QList<Job*> jobs = m_jobs;
    for (Job* job : jobs) {
        if (job->full) {
            vela_capture_end(job->capture);
            m_jobs.removeOne(job);
            delete job;
        }
    }
    m_full.clear();
}

/** Queue complete output images for the interactive Snipping Tool. */
int WindowCapture::captureScreensFull()
{
    if (!m_shm || !m_outputSources || !m_copier) {
        qWarning("vela-shell: the compositor doesn't offer output capture");
        return 0;
    }
    int count = 0;
    for (QScreen* screen : QGuiApplication::screens()) {
        auto* native = screen->nativeInterface<QNativeInterface::QWaylandScreen>();
        if (!native || !native->output()) {
            continue;
        }
        auto* job = new Job;
        job->owner = this;
        job->full = true;
        job->identifier = screen->name();
        auto* source = ext_output_image_capture_source_manager_v1_create_source(m_outputSources, native->output());
        job->capture = vela_capture_begin(m_shm, m_copier, source, &CaptureCallbacks::captured, job);
        if (!job->capture) {
            if (source) ext_image_capture_source_v1_destroy(source);
            delete job;
            continue;
        }
        m_jobs.append(job);
        ++count;
    }
    if (auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
        wl_display_flush(wayland->display());
    }
    return count;
}

/** Copy validated SHM pixels into Qt-owned memory before freeing the job. */
void WindowCapture::finishJob(Job* job, bool ok)
{
    const vela_capture_frame* frame = vela_capture_pixels(job->capture);
    // Capture buffers are in raw orientation. Normalize once before Qt reads
    // thumbnails or the Snipping Tool's full-size images.
    if (ok) {
        QImage full = uprightImage(frame);
        if (!full.isNull()) {
            if (job->full) {
                m_full.insert(job->identifier, full.convertToFormat(QImage::Format_RGB32));
            } else {
                m_thumbnails.insert(job->identifier,
                    full.scaled(thumbnailSize, thumbnailSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    vela_capture_end(job->capture);
    const QString identifier = job->identifier;
    const bool wasFull = job->full;
    m_jobs.removeOne(job);
    delete job;
    if (wasFull) emit screenCaptured(identifier, ok);
    else if (ok) emit thumbnailReady(identifier);
}
