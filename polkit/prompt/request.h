// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The request the prompt shows, as the agent describes it on stdin
// (docs/polkit-agent.md §5); what the user does goes to stdout. From QML it is
// the Request singleton.

#include "../protocol.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QSocketNotifier>
#include <QVariantList>

class Request : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString actionId READ actionId NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString iconName READ iconName NOTIFY changed)
    Q_PROPERTY(QString appName READ appName NOTIFY changed)
    Q_PROPERTY(QString appIcon READ appIcon NOTIFY changed)
    // [{key, value}], for "Show more details".
    Q_PROPERTY(QVariantList details READ details NOTIFY changed)
    // [{uid, login, name, avatar}]: the first is selected at the start.
    Q_PROPERTY(QVariantList identities READ identities NOTIFY changed)
    Q_PROPERTY(int selectedUid READ selectedUid NOTIFY selectedUidChanged)
    Q_PROPERTY(int currentUid READ currentUid CONSTANT) // who we are
    // A choice between identities only if the current user isn't one of them.
    Q_PROPERTY(bool chooseIdentity READ chooseIdentity NOTIFY changed)
    // Everything arrived ("show"): the prompt appears.
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)

    // PAM's current question.
    Q_PROPERTY(bool asking READ asking NOTIFY conversationChanged)
    Q_PROPERTY(QString promptText READ promptText NOTIFY conversationChanged)
    Q_PROPERTY(bool echo READ echo NOTIFY conversationChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY conversationChanged)
    Q_PROPERTY(QString infoText READ infoText NOTIFY conversationChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY conversationChanged)
    // Authentication isn't available (the agent sent "failed").
    Q_PROPERTY(bool unavailable READ unavailable NOTIFY conversationChanged)

    // The prompt is fading out: it exits shortly.
    Q_PROPERTY(bool closing READ closing NOTIFY closingChanged)
    // Caps Lock is on, from the keyboard's real state (capslock.h,
    // docs/keyboard.md §8.2).
    Q_PROPERTY(bool capsLock READ capsLock NOTIFY capsLockChanged)

public:
    // No default constructor: QML must use create(), that is this
    // instance, not build one of its own.
    explicit Request(QObject* parent);

    static Request* instance;
    static Request* create(QQmlEngine*, QJSEngine* engine);

    // Starts reading stdin.
    void listen();

    QString actionId() const { return m_actionId; }
    QString message() const { return m_message; }
    QString iconName() const { return m_iconName; }
    QString appName() const { return m_appName; }
    QString appIcon() const { return m_appIcon; }
    QVariantList details() const { return m_details; }
    QVariantList identities() const { return m_identities; }
    int selectedUid() const { return m_selectedUid; }
    int currentUid() const;
    bool chooseIdentity() const;
    bool shown() const { return m_shown; }
    bool asking() const { return m_asking; }
    QString promptText() const { return m_promptText; }
    bool echo() const { return m_echo; }
    bool checking() const { return m_checking; }
    QString infoText() const { return m_infoText; }
    QString errorText() const { return m_errorText; }
    bool unavailable() const { return m_unavailable; }
    bool closing() const { return m_closing; }
    bool capsLock() const { return m_capsLock; }
    void setCapsLock(bool on);

    // From the interface.
    Q_INVOKABLE void respond(const QString& answer);
    Q_INVOKABLE void selectIdentity(int uid);
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void selectedUidChanged();
    void shownChanged();
    void conversationChanged();
    void closingChanged();
    void capsLockChanged();
    // Wrong answer: the field empties and shakes.
    void retried();
    // A new question from PAM: the field empties and takes the focus.
    void asked();

private:
    void onReadable();
    void handle(const std::string& word, const std::string& argument);
    void write(const std::string& line);
    void close();

    QSocketNotifier* m_notifier = nullptr;
    vela::polkit::LineBuffer m_buffer;

    QString m_actionId;
    QString m_message;
    QString m_iconName;
    QString m_appName;
    QString m_appIcon;
    QVariantList m_details;
    QVariantList m_identities;
    int m_selectedUid = -1;
    bool m_shown = false;
    bool m_asking = false;
    QString m_promptText;
    bool m_echo = false;
    bool m_capsLock = false;
    bool m_checking = false;
    QString m_infoText;
    QString m_errorText;
    bool m_errorSinceResponse = false;
    bool m_unavailable = false;
    bool m_closing = false;
};
