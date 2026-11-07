// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

// The shell's file dialogs, for the desktop and Explorer:
// - Share, like Windows 11: to nearby devices (phones and computers paired
//   with KDE Connect) and by mail (xdg-email);
// - "Choose another app": open with any app, and make it the default for
//   that file type (mimeapps.h);
// - New > Shortcut (links.h).
class FileActions : public QObject {
    Q_OBJECT
    // The reachable KDE Connect devices: [{id, name}].
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY devicesChanged)

public:
    explicit FileActions(QObject* parent = nullptr);

    QVariantList devices() const { return m_devices; }
    bool searching() const { return m_searching; }

    Q_INVOKABLE bool canKdeConnect() const;
    Q_INVOKABLE bool canEmail() const;
    Q_INVOKABLE void findDevices();
    Q_INVOKABLE void sendToDevice(const QString& id, const QStringList& paths);
    Q_INVOKABLE void sendByEmail(const QStringList& paths);

    // The file type ("PNG image") and its extension (".png").
    Q_INVOKABLE QString typeName(const QString& path) const;
    Q_INVOKABLE QString iconFor(const QString& path) const;
    Q_INVOKABLE QString extension(const QString& path) const;
    // `appId` (a .desktop) becomes the default app for `path`'s type.
    Q_INVOKABLE void setDefaultApp(const QString& appId, const QString& path) const;

    // New > Shortcut: empty when done, otherwise the error message.
    Q_INVOKABLE QString createShortcut(const QString& directory, const QString& target, const QString& name) const;
    // The proposed name for the shortcut to `target` inside `directory`.
    Q_INVOKABLE QString shortcutName(const QString& target, const QString& directory) const;
    // "Create shortcut" and "Paste shortcut" on the desktop (or in
    // `directory`).
    Q_INVOKABLE void createLinks(const QStringList& paths, const QString& directory) const;
    Q_INVOKABLE bool pasteLinks(const QString& directory) const;

    // Favorites (the section of Explorer's Home).
    Q_INVOKABLE bool isFavorite(const QString& path) const;
    Q_INVOKABLE void setFavorite(const QString& path, bool on) const;

signals:
    void devicesChanged();

private:
    QVariantList m_devices;
    bool m_searching = false;
};
