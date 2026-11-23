// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QImage>
#include <QObject>
#include <QQuickImageProvider>
#include <QVariantList>

#include <atomic>

class QThread;

struct wl_event_queue;
struct wl_registry;
struct ext_data_control_manager_v1;
struct ext_data_control_device_v1;
struct ext_data_control_offer_v1;
struct ext_data_control_source_v1;

// Clipboard history (Win+V), like Windows 11: what is copied (text and images)
// goes into a list; a click puts it back on the clipboard and pastes it into
// the focused app. Pinned items survive restarts
// (~/.local/share/vela/clipboard), the others last only for the session.
// Passwords copied from password managers aren't remembered.
//
// The clipboard is read and written with the ext-data-control protocol (the
// shell has no focused window), on libwayland directly, on Qt's connection but
// with an event queue of its own, served by a thread: when the shell itself
// (Qt) reads the clipboard we set, the main thread waits for the data and
// couldn't send it. The Snipping Tool's images go through here too.
class Clipboard : public QObject {
    Q_OBJECT
    // [{id, kind ("text"/"image"), text, pinned, time}], most recent first
    // (pinned first, like Windows).
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    // "Clipboard history" (Settings > System > Clipboard).
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit Clipboard(QObject* parent = nullptr);
    ~Clipboard() override;

    QVariantList items() const;
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    // Puts the item back on the clipboard and asks the compositor to paste it.
    Q_INVOKABLE void paste(int id);
    Q_INVOKABLE void remove(int id);
    Q_INVOKABLE void togglePin(int id);
    Q_INVOKABLE void clear(); // everything but pinned items

    // On the clipboard (and in the history, if on).
    void copyText(const QString& text);
    void copyImage(const QImage& image);

    QImage image(int id, const QSize& requested) const;

signals:
    void itemsChanged();
    void enabledChanged();

private:
    struct Item {
        int id = 0;
        QString text;
        QImage preview;
        QSize originalSize;
        bool setImage(const QImage& image, QByteArray originalPng = { });
        bool sameImage(const Item& other) const;
        QByteArray png; // the image as it arrived (or encoded), to put it back
        bool pinned = false;
        QDateTime time;
    };
    struct Offer;
    friend struct ClipboardCallbacks;

    void add(Item item);
    void setSelection(const Item& item);
    void readOffer(Offer* offer); // in the clipboard thread
    void savePinned() const;
    void loadPinned();
    void flush();
    void run(); // the thread: serves the clipboard queue

    wl_event_queue* m_queue = nullptr;
    QThread* m_thread = nullptr;
    std::atomic<bool> m_stop { false };
    int m_wake[2] { -1, -1 }; // to wake the thread when closing
    QMutex m_mutex; // m_sourceText and m_sourcePng, read by the thread
    wl_registry* m_registry = nullptr;
    ext_data_control_manager_v1* m_manager = nullptr;
    ext_data_control_device_v1* m_device = nullptr;
    QByteArray m_sourceText;
    QByteArray m_sourcePng;
    QHash<ext_data_control_offer_v1*, Offer*> m_offers; // only in the thread
    std::atomic<bool> m_ignoreNext { false }; // the selection we set ourselves
    mutable QMutex m_itemsMutex; // asynchronous image-provider reads vs. history edits
    QList<Item> m_items;
    int m_nextId = 1;
    std::atomic<bool> m_enabled { false };
};

// History images for QML: "image://clipboard/<id>".
class ClipboardImageProvider : public QQuickImageProvider {
public:
    explicit ClipboardImageProvider(Clipboard* clipboard)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_clipboard(clipboard)
    {
    }
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override
    {
        const QImage image = m_clipboard->image(id.section(u'/', 0, 0).toInt(), requested);
        if (size) {
            *size = image.size();
        }
        return image;
    }

private:
    Clipboard* m_clipboard;
};
