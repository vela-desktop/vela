// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QVariantMap>

class QDBusMessage;
class QDBusServiceWatcher;

class LauncherEntry : public QObject {
    Q_OBJECT

public:
    struct Badge {
        qlonglong count = 0;
        double progress = -1;
        bool urgent = false;
    };

    explicit LauncherEntry(QObject* parent = nullptr);
    Badge badge(const QString& desktopId) const;

signals:
    void changed();

private slots:
    void update(const QString& uri, const QVariantMap& properties, const QDBusMessage& message);
    void removeSender(const QString& sender);

private:
    struct Entry {
        qlonglong count = 0;
        double progress = 0;
        bool countVisible = true;
        bool progressVisible = false;
        bool urgent = false;
        quint64 updated = 0;
    };
    QHash<QString, QHash<QString, Entry>> m_entries; // desktop id, then unique bus name
    QDBusServiceWatcher* m_watcher;
    quint64 m_revision = 0;
};
