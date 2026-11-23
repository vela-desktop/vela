// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QVariantList>

// Personalization > Taskbar > Notification area (docs/taskbar.md §8.2): the
// notification area icons of the apps running now, read from the
// StatusNotifierWatcher the shell owns. Each one: { key, title, icon }, the
// key as the shell saves it in taskbar/trayShown (the item's Id, else its
// Title).
class TrayIcons : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)

public:
    explicit TrayIcons(QObject* parent = nullptr);

    QVariantList items() const { return m_items; }
    Q_INVOKABLE void refresh();

signals:
    void itemsChanged();

private slots:
    void onRegistered(const QString& address);

private:
    QVariantList m_items;
    int m_generation = 0;
};
