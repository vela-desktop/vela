// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>

#include <functional>

// Explorer's places, like Windows 11: Quick access (the user's folders and the
// added ones, remembered), Favorites (files), the drives of "This PC" with
// their free space, also those not mounted yet (from udisks: they're mounted
// when opened), recent files (recently-used.xbel, the same as KDE and GNOME)
// and the Recycle Bin.
class Places : public QObject {
    Q_OBJECT
    // [{name, path, icon, pinned}]
    Q_PROPERTY(QVariantList quickAccess READ quickAccess NOTIFY quickAccessChanged)
    // [{name, path, icon, total, free, device, removable, mounted, volume}]:
    // unmounted ones have an empty path and `volume` (the udisks object).
    Q_PROPERTY(QVariantList drives READ drives NOTIFY drivesChanged)
    // [{name, path, icon, modified, location}]
    Q_PROPERTY(QVariantList favorites READ favorites NOTIFY favoritesChanged)
    Q_PROPERTY(QString home READ home CONSTANT)
    Q_PROPERTY(QString trash READ trash CONSTANT) // the folder of the files in the Recycle Bin
    Q_PROPERTY(QString userName READ userName CONSTANT)

public:
    explicit Places(QObject* parent = nullptr);
    void setTransferGuard(std::function<bool(const QString&)> guard);
    QStringList ejectingDevices() const { return m_ejecting.values(); }

    QVariantList quickAccess() const { return m_quickAccess; }
    QVariantList drives() const { return m_drives; }
    QString home() const;
    QString trash() const;
    QString userName() const;

    Q_INVOKABLE void refreshDrives();
    // Mounts a drive (udisks, with the password if needed): then `mounted`.
    Q_INVOKABLE void mount(const QString& volume);
    // Eject: unmounts and, when possible, powers the device off (sticks, USB
    // disks).
    Q_INVOKABLE void eject(const QString& device);

    QVariantList favorites() const;
    Q_INVOKABLE bool isFavorite(const QString& path) const;
    Q_INVOKABLE void setFavorite(const QString& path, bool on);
    Q_INVOKABLE bool isPinned(const QString& path) const;
    Q_INVOKABLE void pin(const QString& path);
    Q_INVOKABLE void unpin(const QString& path);
    // The recently used files: [{name, path, icon, modified, location}], most
    // recent first.
    Q_INVOKABLE QVariantList recentFiles(int limit) const;
    // The name to show for a folder ("Documents", "Local Disk"...).
    Q_INVOKABLE QString displayName(const QString& path) const;
    Q_INVOKABLE QString iconFor(const QString& path) const;

private Q_SLOTS:
    void onVolumesChanged(); // udisks: a disk plugged in, unplugged, mounted

signals:
    void quickAccessChanged();
    void drivesChanged();
    void favoritesChanged();
    // Mounted (where), or it couldn't be (error not empty).
    void mounted(const QString& volume, const QString& path, const QString& error);
    void ejected(const QString& error);

private:
    void loadQuickAccess();
    void saveQuickAccess();
    void readVolumes(); // udisks, in the background
    void publishDrives();

    QStringList m_pinned;
    QVariantList m_quickAccess;
    QVariantList m_drives;
    QVariantList m_mountedDrives; // from /proc/mounts
    QVariantList m_volumes; // from udisks, not mounted
    // device (/dev/...) -> {block: object, drive: the disk's object,
    // removable}
    QHash<QString, QVariantMap> m_devices;
    std::function<bool(const QString&)> m_transferGuard;
    QSet<QString> m_ejecting; // includes sibling partitions until UDisks completes
    bool m_watchingVolumes = false;
    bool m_readingVolumes = false;
};
