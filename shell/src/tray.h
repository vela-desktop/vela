// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The notification area (tray): the icons apps put next to the clock
// (Telegram, Discord, Steam...). The protocol is StatusNotifierItem (D-Bus),
// with menus in com.canonical.dbusmenu.
//
// - The "watcher" is the registry where apps announce their icons. If
//   nobody offers it already (Plasma, when Vela runs inside KDE), the shell
//   does.
// - The "host" is whoever shows the icons: this model, for QML.

#include <QAbstractListModel>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QImage>
#include <QQuickImageProvider>
#include <QStringList>

#include <memory>
#include <vector>

class QDBusServiceWatcher;
class QDBusPendingCallWatcher;

// The registry, on the bus as org.kde.StatusNotifierWatcher.
class StatusNotifierWatcher : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
    // D-Bus properties: their changes go out as the watcher's own signals.
    Q_PROPERTY(QStringList RegisteredStatusNotifierItems READ items) // clazy:exclude=qproperty-without-notify
    Q_PROPERTY(bool IsStatusNotifierHostRegistered READ hostRegistered) // clazy:exclude=qproperty-without-notify
    Q_PROPERTY(int ProtocolVersion READ protocolVersion CONSTANT)

public:
    explicit StatusNotifierWatcher(QObject* parent = nullptr);
    bool registerService(); // false if there's another registry already

    QStringList items() const { return m_items; }
    bool hostRegistered() const { return true; }
    int protocolVersion() const { return 0; }

public Q_SLOTS:
    Q_SCRIPTABLE void RegisterStatusNotifierItem(const QString& service);
    Q_SCRIPTABLE void RegisterStatusNotifierHost(const QString& service);

Q_SIGNALS:
    Q_SCRIPTABLE void StatusNotifierItemRegistered(const QString& service);
    Q_SCRIPTABLE void StatusNotifierItemUnregistered(const QString& service);
    Q_SCRIPTABLE void StatusNotifierHostRegistered();

private:
    QStringList m_items; // "service/path"
    QDBusServiceWatcher* m_owners;
};

class TrayModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)

public:
    enum Role {
        IconRole = Qt::UserRole + 1,
        TitleRole,
        AttentionRole, // asks for attention (such as unread messages)
    };

    explicit TrayModel(QObject* parent = nullptr);
    ~TrayModel() override;

    // Connects to the registry (ours or another's) and loads the icons.
    void start();

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QVariantList entries() const;

    // From QML. anchorX: the icon's center on screen, where to open the menu.
    Q_INVOKABLE void activate(int row, int anchorX); // left click (or menu, if the icon is only a menu)
    Q_INVOKABLE void secondaryActivate(int row); // middle click
    Q_INVOKABLE void scroll(int row, int delta);
    // Asks for the icon's menu: it arrives with menuReady.
    // With `always`, menuReady comes even when the app has no menu (empty):
    // a right click shows the shell's own entries.
    Q_INVOKABLE void requestMenu(int row, int anchorX, bool always = false);
    // An entry of the shown menu was chosen.
    Q_INVOKABLE void activateMenuEntry(int row, int entryId);

    QImage pixmap(const QString& key) const;

Q_SIGNALS:
    void entriesChanged();
    // entries: [{ id, label, enabled, separator, checkable, checked, radio,
    // icon, children }]
    void menuReady(int row, int anchorX, const QVariantList& entries);

private:
    struct Item;
    void addItem(const QString& address);
    void removeItem(const QString& address);
    void refresh(Item* item);
    int rowOf(const Item* item) const;
    Item* itemAt(int row) const;

    std::vector<std::unique_ptr<Item>> m_items;
    StatusNotifierWatcher* m_watcher = nullptr; // if the registry is ours
    uint m_serial = 0;

private Q_SLOTS:
    // From another's registry (Plasma), over D-Bus.
    void onItemRegistered(const QString& address) { addItem(address); }
    void onItemUnregistered(const QString& address) { removeItem(address); }
};

// "image://tray/<key>": the icons drawn by apps (IconPixmap).
class TrayImageProvider : public QQuickImageProvider {
public:
    explicit TrayImageProvider(TrayModel* tray)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_tray(tray)
    {
    }
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    TrayModel* m_tray;
};
