// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QQuickImageProvider>
#include <QString>
#include <QStringList>
#include <QVariantList>

struct vela_capture_job;
struct wl_registry;
struct wl_shm;
struct ext_foreign_toplevel_list_v1;
struct ext_foreign_toplevel_handle_v1;
struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct ext_image_copy_capture_manager_v1;

// Window previews (for Alt+Tab), with the standard ext-foreign-toplevel-list
// (every window has an identifier) and ext-image-copy-capture protocols. Each
// capture is a photo: taken when the switcher opens, then only the thumbnail
// is kept.
//
// It uses libwayland directly, on Qt's connection: events arrive on the
// default queue, which Qt dispatches in the main thread.
class WindowCapture : public QObject {
    Q_OBJECT

public:
    explicit WindowCapture(QObject* parent = nullptr);
    ~WindowCapture() override;

    Q_INVOKABLE void capture(const QStringList& identifiers);
    // A photo of each output: the thumbnails are called "screen:<name>".
    Q_INVOKABLE void captureScreens();
    // The same at full resolution (the Snipping Tool): then screenCaptured and
    // fullImage(name). The number of outputs captured.
    int captureScreensFull();
    QImage fullImage(const QString& screen) const { return m_full.value(screen); }
    void clearFull() { m_full.clear(); }
    // The open windows: [{id, title, appId}], in opening order.
    Q_INVOKABLE QVariantList windowList() const;
    Q_INVOKABLE QString title(const QString& identifier) const;
    Q_INVOKABLE QString appId(const QString& identifier) const;
    QImage thumbnail(const QString& identifier) const { return m_thumbnails.value(identifier); }

    // Longest side of the thumbnails, in pixels.
    static constexpr int thumbnailSize = 400;

signals:
    void thumbnailReady(const QString& identifier);
    void screenCaptured(const QString& screen, bool ok);

private:
    struct Window;
    struct Job;
    friend struct CaptureCallbacks;

    void finishJob(Job* job, bool ok);

    wl_registry* m_registry = nullptr;
    wl_shm* m_shm = nullptr;
    ext_foreign_toplevel_list_v1* m_list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* m_sources = nullptr;
    ext_output_image_capture_source_manager_v1* m_outputSources = nullptr;
    ext_image_copy_capture_manager_v1* m_copier = nullptr;

    QHash<ext_foreign_toplevel_handle_v1*, Window*> m_windows;
    QHash<QString, QImage> m_thumbnails;
    QHash<QString, QImage> m_full; // output -> whole image
    QList<Job*> m_jobs;
};

// Thumbnails for QML: "image://thumbnail/<identifier>/<version>" (the version
// changes at every new capture, so Qt doesn't use the old one).
class ThumbnailProvider : public QQuickImageProvider {
public:
    explicit ThumbnailProvider(WindowCapture* capture)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_capture(capture)
    {
    }

    QImage requestImage(const QString& id, QSize* size, const QSize&) override
    {
        const QImage image = m_capture->thumbnail(id.section(u'/', 0, 0));
        if (size) {
            *size = image.size();
        }
        return image;
    }

private:
    WindowCapture* m_capture;
};
