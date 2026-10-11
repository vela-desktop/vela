// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

// The list of installed applications, read from the standard .desktop files
// (the same ones KDE, GNOME and the rest of the Linux world use).
class AppModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Changes even when an installed app is renamed or replaced without changing count.
    Q_PROPERTY(int catalogRevision READ catalogRevision NOTIFY catalogRevisionChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        IconRole,
        CommentRole,
    };

    // An action declared by the app ([Desktop Action ...]): "New window"...
    struct Action {
        QString id;
        QString name;
        QString icon;
        QString exec; // with the field codes
    };

    struct Entry {
        QString id; // such as org.kde.dolphin.desktop
        QString path; // the .desktop file
        QString rawExec; // with the field codes (%f, %u...)
        QList<Action> actions;
        QStringList mimeTypes; // the file types it can open
        QString name;
        QString genericName;
        QString comment;
        QString keywords;
        QString icon;
        QString exec; // without field codes: for searching
        QString program; // the program name (such as "kate"), from Exec's first argument
        QString workDir; // Path=: the directory to start it in (empty: home)
        QString wmClass; // StartupWMClass: its windows' app_id, if different from the id
        bool terminal = false;
    };

    explicit AppModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return m_query; }
    void setQuery(const QString& query);
    int count() const { return static_cast<int>(m_visible.size()); }
    int catalogRevision() const { return m_catalogRevision; }

    Q_INVOKABLE void reload();
    Q_INVOKABLE bool launch(int row);
    Q_INVOKABLE bool launchId(const QString& id);
    // A single app's data by id (for the icons pinned to the taskbar).
    Q_INVOKABLE QVariantMap entry(const QString& id) const;
    // The app's actions for the jump list: [{id, name, icon}].
    Q_INVOKABLE QVariantList actions(const QString& id) const;
    Q_INVOKABLE bool launchAction(const QString& id, const QString& actionId);
    // Opens a file (or a URL) with that app.
    Q_INVOKABLE bool launchWithFile(const QString& id, const QString& url);
    Q_INVOKABLE QString desktopFile(const QString& id) const;
    Q_INVOKABLE QString name(const QString& id) const;
    // The program the app starts (such as "kate"), to recognize it elsewhere.
    QString program(const QString& id) const;
    const Entry* find(const QString& id) const;
    // "Open with": the apps that can open that file type (and its parents,
    // such as text/plain for C++), the default first: [{id, name, icon,
    // isDefault}].
    Q_INVOKABLE QVariantList appsForFile(const QString& path) const;
    // All apps, by name: [{id, name, icon}] ("Choose another app").
    Q_INVOKABLE QVariantList allApps() const;
    // A .desktop shortcut outside the menu (such as on the desktop): starts
    // it.
    Q_INVOKABLE bool launchDesktopFile(const QString& path) const;
    // The menu id of a .desktop file found elsewhere (such as steam.desktop on
    // the desktop), if it's the same app; empty otherwise.
    Q_INVOKABLE QString idForDesktopFile(const QString& path) const;
    const QList<Entry>& all() const { return m_all; }
    // The .desktop file of an open window, from its app_id (such as
    // "org.kde.konsole" -> "org.kde.konsole.desktop"). Empty if unknown.
    QString findDesktopId(const QString& appId) const;
    // The icon of an open window, from its app_id.
    Q_INVOKABLE QString iconForAppId(const QString& appId) const;

signals:
    void queryChanged();
    void countChanged();
    void catalogRevisionChanged();

private:
    QList<int> filteredIndices() const;
    void applyFilter();
    // Keep file and directory watches in sync after installs, removals and
    // atomic replacements. Missing XDG directories are watched via their
    // nearest existing parent until the directory is created.
    void updateWatchPaths(const QStringList& directories, const QSet<QString>& desktopFiles);
    // Starts the app with the given files (possibly none), as Exec says
    // (desktopexec.h): the program with its arguments, without going through
    // the shell.
    bool launchEntry(const Entry& entry, const QList<QUrl>& files = {}) const;

    QList<Entry> m_all; // sorted by name
    QList<int> m_visible; // indexes in m_all matching the search
    QString m_query;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    // Retry failed watch registrations at most once per 10 seconds.
    QTimer m_watchRetryTimer;
    QSet<QString> m_failedWatchPaths;
    int m_catalogRevision = 0;
};

// The default app for a MIME type (.desktop id), from mimeapps.list.
QString defaultAppFor(const QString& mime);
