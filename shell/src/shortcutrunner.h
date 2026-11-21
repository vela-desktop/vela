// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The user's custom shortcuts, run (docs/keyboard.md §9.5): the compositor
// matches the combination and sends only "shortcut <combination>"; this
// reads $XDG_CONFIG_HOME/vela/shortcuts.conf again, finds the line and runs
// it as Start would: an app by its desktop file, a command with sh -c, a
// link or file with its default app.

#include <QObject>
#include <QString>

class AppModel;

class ShortcutRunner : public QObject {
    Q_OBJECT

public:
    explicit ShortcutRunner(AppModel* apps, QObject* parent = nullptr);

    // What the line for `combo` asks: {kind, target}, empty if none.
    struct Target {
        QString kind;
        QString target;
    };
    static Target find(const QString& path, const QString& combo);

public slots:
    // false if no line has it (the file changed in between) or it couldn't
    // start.
    bool run(const QString& combo);

private:
    AppModel* m_apps;
};
