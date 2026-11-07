// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QWindow>

class AppModel;
class ForeignToplevel;
class ForeignToplevelManager;

// The taskbar buttons, like Windows 11: pinned apps first (open or not), then
// the other open apps, in the order they were started. Windows of the same app
// are under a single button.
class TaskbarModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QStringList pinnedIds READ pinnedIds WRITE setPinnedIds NOTIFY pinnedIdsChanged)
    // Pinned apps have already been saved (otherwise the defaults are used).
    Q_PROPERTY(bool pinsSaved READ pinsSaved CONSTANT)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        NameRole,
        IconRole,
        PinnedRole,
        WindowCountRole,
        WindowActiveRole,
        DesktopIdRole,
    };

    TaskbarModel(AppModel* apps, ForeignToplevelManager* windows, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QStringList pinnedIds() const { return m_pinnedIds; }
    void setPinnedIds(const QStringList& ids);
    bool pinsSaved() const { return m_pinsSaved; }
    Q_INVOKABLE bool isPinned(const QString& desktopId) const { return m_pinnedIds.contains(desktopId); }
    Q_INVOKABLE void pin(const QString& desktopId);
    Q_INVOKABLE void unpin(const QString& desktopId);

    // Click on the button: starts the app, or brings its window to the front,
    // or (if it's already the active one) minimizes it.
    Q_INVOKABLE void activate(int row);
    // The app's most recently used window to the front, never minimized nor
    // launched: false when it has none (the screen sharing menu).
    Q_INVOKABLE bool showApp(const QString& desktopId);
    // The button dragged elsewhere: pinned apps stay before the ones only
    // open, and their order is remembered.
    Q_INVOKABLE void move(int from, int to);
    // Middle click: a new window of the app.
    Q_INVOKABLE void launchNew(int row);
    // "Close window" / "Close all windows".
    Q_INVOKABLE void closeWindows(int row);
    // "End task": the compositor kills the processes of its windows.
    Q_INVOKABLE void endTask(int row);
    // The window menu from the button (Shift+right click), on its most recent
    // window: {maximized, minimized}; and its actions (restore, move, resize,
    // minimize, maximize, close).
    Q_INVOKABLE QVariantMap windowState(int row) const;
    Q_INVOKABLE void windowAction(int row, const QString& action);
    // Win+D and "Desktop" in the Win+X menu: minimizes everything, and the
    // next time puts it back.
    Q_INVOKABLE void toggleDesktop();
    // The app_ids of its open windows (to find them in previews).
    Q_INVOKABLE QStringList appIds(int row) const;
    // The button's position in the taskbar window.
    Q_INVOKABLE void setButtonGeometry(int row, QWindow* panel, const QRectF& rect);

signals:
    void pinnedIdsChanged();

private:
    struct Item {
        QString key; // .desktop id, or "app:<app_id>" if unknown
        QString desktopId;
        QString name;
        QString icon;
        bool pinned = false;
        QList<ForeignToplevel*> windows;
    };

    void rebuild();
    QList<Item> buildItems();
    ForeignToplevel* recentWindow(int row) const;
    void sendButtonRects();

    AppModel* m_apps;
    ForeignToplevelManager* m_windows;
    QStringList m_pinnedIds;
    QStringList m_unpinnedOrder; // keys of unpinned apps, in order of appearance
    QList<Item> m_items;
    QPointer<QWindow> m_panel;
    QHash<QString, QRect> m_buttonRects; // by app key
    bool m_pinsSaved = false;
    QList<QPointer<ForeignToplevel>> m_hiddenByDesktop; // minimized by "Show desktop"
};
