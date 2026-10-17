// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The desktop notification server (org.freedesktop.Notifications): apps send
// messages here, the shell shows them at the bottom right like Windows 11. For
// QML it's a model: one row per visible notification.

#include <QAbstractListModel>
#include <QDateTime>
#include <QDBusContext>
#include <QHash>
#include <QImage>
#include <QQuickImageProvider>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <memory>
#include <vector>

// The notification center (Win+N): past notifications, most recent first,
// until dismissed. Those expired from the popup end up here (not "transient"
// ones) and, with "Do not disturb", all non-critical ones.
class NotificationHistory : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    using QAbstractListModel::QAbstractListModel;
    int count() const { return int(m_items.size()); }
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    friend class NotificationServer;
    struct Item {
        uint id;
        QString appName;
        QString icon;
        QString summary;
        QString body;
        QVariantList actions;
        bool hasDefault;
        QDateTime time;
    };
    std::vector<Item> m_items; // most recent at the top
};

class NotificationServer : public QAbstractListModel, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QObject* history READ history CONSTANT)
    // "Do not disturb": no popups (except critical ones), everything in the
    // center.
    Q_PROPERTY(bool doNotDisturb READ doNotDisturb WRITE setDoNotDisturb NOTIFY doNotDisturbChanged)
    // The screen is being shared or recorded and the user asked for silence:
    // popups are held back like with doNotDisturb, which stays the user's own.
    Q_PROPERTY(bool captureSilenced READ captureSilenced NOTIFY silencedChanged)
    // doNotDisturb OR captureSilenced: what the popups obey.
    Q_PROPERTY(bool silenced READ silenced NOTIFY silencedChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        AppNameRole,
        IconRole, // source for Image: theme, file or the notification's image
        SummaryRole,
        BodyRole,
        ActionsRole, // [{ key, label }], without the "default" action
        HasDefaultActionRole,
        CriticalRole,
    };

    explicit NotificationServer(QObject* parent = nullptr);
    ~NotificationServer() override;

    // Takes the name on the session bus. false if there is another server
    // already (Plasma, when Vela runs nested or from a console).
    bool registerService();

    int count() const { return int(m_items.size()); }
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // From QML.
    Q_INVOKABLE void invoke(uint id, const QString& action); // "default" for the click
    Q_INVOKABLE void dismiss(uint id);
    // The mouse is over the notifications: they don't expire until it leaves.
    Q_INVOKABLE void setHovered(bool hovered);
    // The notification center opens: popups still on screen move into it, like
    // on Windows (before they stayed on top, covering the calendar).
    Q_INVOKABLE void collectPopups();

    // The notification center.
    QObject* history() { return &m_history; }
    bool doNotDisturb() const { return m_doNotDisturb; }
    void setDoNotDisturb(bool on);
    bool captureSilenced() const { return m_captureSilenced; }
    bool silenced() const { return m_doNotDisturb || m_captureSilenced; }
    void setCaptureSilenced(bool on);
    Q_INVOKABLE void invokeFromHistory(uint id, const QString& action);
    Q_INVOKABLE void dismissFromHistory(uint id);
    Q_INVOKABLE void clearHistory();

    QImage image(uint id) const { return m_images.value(id); }

    // --- D-Bus ---
public Q_SLOTS:
    Q_SCRIPTABLE QStringList GetCapabilities();
    Q_SCRIPTABLE uint Notify(const QString& app_name, uint replaces_id, const QString& app_icon,
        const QString& summary, const QString& body, const QStringList& actions, const QVariantMap& hints,
        int expire_timeout);
    Q_SCRIPTABLE void CloseNotification(uint id);
    Q_SCRIPTABLE QString GetServerInformation(QString& vendor, QString& version, QString& spec_version);

Q_SIGNALS:
    void countChanged();
    void doNotDisturbChanged();
    void silencedChanged();
    Q_SCRIPTABLE void NotificationClosed(uint id, uint reason);
    Q_SCRIPTABLE void ActionInvoked(uint id, const QString& action_key);

private:
    struct Notification {
        uint id;
        QString appName;
        QString icon;
        QString summary;
        QString body;
        QVariantList actions;
        bool hasDefault;
        bool critical;
        bool transient; // doesn't go to the notification center
        std::unique_ptr<QTimer> timer; // expiry; null: stays until dismissed
    };
    void toHistory(const Notification& n);
    int historyRow(uint id) const;
    enum class Reason : uint { Expired = 1, Dismissed = 2, Closed = 3 };

    void close(uint id, Reason reason);
    int rowOf(uint id) const;
    QString iconFor(uint id, const QString& appIcon, const QVariantMap& hints);

    std::vector<Notification> m_items; // most recent at the end
    QHash<uint, QImage> m_images;
    uint m_nextId = 1;
    bool m_hovered = false;
    bool m_doNotDisturb = false;
    bool m_captureSilenced = false; // not remembered: follows the sharing
    NotificationHistory m_history;
};

// "image://notification/<id>": the image attached to a notification.
class NotificationImageProvider : public QQuickImageProvider {
public:
    explicit NotificationImageProvider(NotificationServer* server)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_server(server)
    {
    }
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    NotificationServer* m_server;
};
