// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

// KDE "service menus" (kio/servicemenus/*.desktop): the entries apps add to
// the file menu (such as Filelight's "Show disk usage statistics"). In Vela
// they're a group of their own in the file and folder menus, before
// Properties (docs/renderer.md §14.9); each one can be hidden in Settings by
// its key (§14.12).
class ServiceMenus : public QObject {
    Q_OBJECT

public:
    explicit ServiceMenus(QObject* parent = nullptr);

    // The entries that apply to these files: [{id, key, text, icon, submenu}].
    // id is for run(); key, "service/<file name>/<action>", stays the same
    // across sessions (the hidden entries in Settings).
    Q_INVOKABLE QVariantList actionsFor(const QStringList& paths);
    // Every entry installed, whatever it applies to: [{key, text, icon,
    // submenu}] (Settings > Personalization > Context menu), read again at
    // each call.
    Q_INVOKABLE QVariantList all();
    Q_INVOKABLE void run(int id, const QStringList& paths) const;

private:
    struct Action {
        QString key;
        QString name;
        QString icon;
        QString exec;
        QString submenu;
        QStringList mimeTypes;
        int minUrls = 1;
        int maxUrls = -1;
        QList<int> requiredUrls;
    };
    void load();
    static QList<Action> scan();

    QList<Action> m_actions;
    bool m_loaded = false;
};
