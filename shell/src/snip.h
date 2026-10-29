// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QJsonObject>
#include <QPointF>
#include <QProcess>
#include <QQuickImageProvider>
#include <QTemporaryFile>
#include <QTimer>
#include <QVariantList>

class Clipboard;
class NotificationServer;
class QQmlEngine;
class QQuickWindow;
class WindowCapture;

// The Snipping Tool (Win+Shift+S, Print), like Windows 11: the screen freezes
// (a photo of each output) and you choose what to keep: a rectangle, a window
// or a whole output. The snip goes to the clipboard (and its history) and to
// Pictures/Screenshots, and a notification says so; a click on the
// notification opens it.
class Snip : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // "rect", "window", "screen" or "text" (a rectangle whose text is copied)
    // Delay before the capture, seconds: 0, 3, 5 or 10.
    Q_PROPERTY(int delay READ delay NOTIFY delayChanged)
    // "record": a rectangle (or a click: the output) is recorded with wf-recorder.
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(int recordSeconds READ recordSeconds NOTIFY recordSecondsChanged)
    Q_PROPERTY(bool microphone READ microphone NOTIFY microphoneChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    // The visible windows, topmost first: [{x, y, w, h, title}] in the layout.
    Q_PROPERTY(QVariantList windows READ windows NOTIFY activeChanged)

public:
    Snip(QQmlEngine* engine, WindowCapture* capture, Clipboard* clipboard, NotificationServer* notifications,
        QObject* parent = nullptr);

    bool active() const { return m_active; }
    int delay() const { return m_delay; }
    Q_INVOKABLE void cycleDelay();
    QString mode() const { return m_mode; }
    void setMode(const QString& mode);
    QVariantList windows() const { return m_windows; }

    bool recording() const { return m_recorder != nullptr; }
    int recordSeconds() const { return m_recordSeconds; }
    bool microphone() const { return m_microphone; }
    Q_INVOKABLE void toggleMicrophone();
    Q_INVOKABLE void stopRecording();

    void start(); // while recording: stops the recording instead
    void startText(); // the Text mode from the start
    Q_INVOKABLE void cancel();
    // The snip, in logical coordinates of output `screen`.
    Q_INVOKABLE void finish(const QString& screen, qreal x, qreal y, qreal width, qreal height);
    // Text mode: finds the words in the rectangle (wordsReady), copyText copies and closes.
    Q_INVOKABLE void recognize(const QString& screen, qreal x, qreal y, qreal width, qreal height);
    Q_INVOKABLE void copyText(const QString& text);
    // Color mode: "#RRGGBB" of the pixel (logical coordinates), "" outside.
    Q_INVOKABLE QString colorAt(const QString& screen, qreal x, qreal y) const;
    Q_INVOKABLE void pickColor(const QString& screen, qreal x, qreal y);

    QImage image(const QString& screen) const;

signals:
    void activeChanged();
    void modeChanged();
    void delayChanged();
    void recordingChanged();
    void recordSecondsChanged();
    void microphoneChanged();
    // [{text, x, y, w, h, line}] in the output's logical coordinates.
    void wordsReady(const QString& screen, const QVariantList& words);
    void recognizeFailed(const QString& screen, const QString& reason);

private:
    void keep(const QImage& picture); // save, clipboard, notification
    void showOverlays();
    void closeOverlays();
    void startWorker();
    void finishRecognize(const QJsonObject& reply);
    void finishNow(const QString& screen, qreal x, qreal y, qreal width, qreal height);
    void tickCountdown();
    void captured();
    void startRecording(const QString& screen, const QRectF& area);
    void launchRecorder(bool webm);
    void recorderFinished(int code, bool crashed);

    QQmlComponent m_component;
    WindowCapture* m_capture;
    Clipboard* m_clipboard;
    NotificationServer* m_notifications;
    bool m_active = false;
    QString m_mode = QStringLiteral("rect");
    int m_waiting = 0;
    int m_delay = 0;
    // The delayed snip: the rectangle waits for the countdown and a new photo.
    struct Delayed {
        bool recapturing = false;
        QString screen;
        QRectF area;
    } m_delayed;
    int m_remaining = 0;
    QTimer m_countdown;
    QPointer<QQuickWindow> m_counter;
    QQmlComponent m_counterComponent;
    QTimer m_captureTimeout;
    QVariantList m_windows;
    QList<QPointer<QQuickWindow>> m_overlays;
    QHash<uint, QString> m_saved; // notification -> saved file

    // wf-recorder, and what it was asked to record.
    QProcess* m_recorder = nullptr;
    QString m_recordFile;
    QString m_recordOutput;
    QRect m_recordArea;
    bool m_recordWebm = false;
    bool m_microphone = false;
    bool m_stopping = false;
    int m_recordSeconds = 0;
    QTimer m_recordClock;
    QPointer<QQuickWindow> m_bar;
    QQmlComponent m_barComponent;

    // The vela-ocr worker and the image it is reading.
    struct Pending {
        QTemporaryFile* file = nullptr;
        QString screen;
        QPointF origin; // of the crop, logical
        QPointF scale; // pixels per logical unit
    };
    QProcess* m_worker = nullptr;
    Pending m_pending;
    QTimer m_idle;
};

// "image://snip/<output>": the photo of the output to snip.
class SnipImageProvider : public QQuickImageProvider {
public:
    explicit SnipImageProvider(Snip* snip)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_snip(snip)
    {
    }
    QImage requestImage(const QString& id, QSize* size, const QSize&) override
    {
        const QImage image = m_snip->image(id.section(u'/', 0, 0));
        if (size) {
            *size = image.size();
        }
        return image;
    }

private:
    Snip* m_snip;
};
