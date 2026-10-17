// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Per-filesystem advisory locks shared by all vela-files processes. Transfers
// take shared locks; eject takes exclusive locks through the async UDisks call.
// flock() releases them automatically if either process crashes.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace vela::files {

inline QString canonicalDevice(const QString& device)
{
    const QString path = QFileInfo(device).canonicalFilePath();
    return path.isEmpty() ? device : path;
}

inline bool pathContains(const QString& parent, const QString& child)
{
    return parent == child || child.startsWith(parent == QLatin1String("/")
        ? QStringLiteral("/") : parent + u'/');
}

// Check the immediate volumes and mountpoints nested below a recursive source
// or destination (folder merges can touch those nested filesystems too).
inline QStringList transferDevices(const QStringList& sources, const QString& destination)
{
    QSet<QString> devices;
    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    QStringList paths = sources;
    paths.append(destination);
    for (const QString& path : paths) {
        const QStorageInfo immediate(path);
        if (immediate.isValid() && !immediate.device().isEmpty()) {
            devices.insert(canonicalDevice(QFile::decodeName(immediate.device())));
        }
        const QFileInfo entry(path);
        if (!entry.isDir() || entry.isSymLink()) {
            continue;
        }
        const QString root = entry.canonicalFilePath();
        if (root.isEmpty()) {
            continue;
        }
        for (const QStorageInfo& volume : volumes) {
            if (!volume.isValid() || volume.device().isEmpty()) {
                continue;
            }
            const QString mount = QFileInfo(volume.rootPath()).canonicalFilePath();
            if (!mount.isEmpty() && pathContains(root, mount)) {
                devices.insert(canonicalDevice(QFile::decodeName(volume.device())));
            }
        }
    }
    QStringList result = devices.values();
    result.sort();
    return result;
}

class DeviceLocks final {
public:
    enum class Mode { Shared, Exclusive };
    DeviceLocks() = default;
    DeviceLocks(const DeviceLocks&) = delete;
    DeviceLocks& operator=(const DeviceLocks&) = delete;
    ~DeviceLocks() { release(); }

    bool acquire(const QStringList& deviceNames, Mode mode)
    {
        release();
        QSet<QString> keys;
        for (const QString& name : deviceNames) {
            if (!name.isEmpty()) {
                keys.insert(canonicalDevice(name));
            }
        }
        if (keys.isEmpty()) {
            return true;
        }
        const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (base.isEmpty() || !QDir().mkpath(base + QStringLiteral("/vela-transfer-locks"))) {
            return false;
        }
        QStringList names = keys.values();
        names.sort(); // Consistent lock order across processes.
        for (const QString& name : names) {
            const QByteArray key = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex();
            const QByteArray file = QFile::encodeName(base + QStringLiteral("/vela-transfer-locks/"))
                + key + ".lock";
            const int fd = ::open(file.constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) {
                release();
                return false;
            }
            if (::flock(fd, (mode == Mode::Shared ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
                ::close(fd);
                release();
                return false;
            }
            m_fds.append(fd);
        }
        return true;
    }

    void release()
    {
        for (const int fd : m_fds) {
            ::close(fd);
        }
        m_fds.clear();
    }

private:
    QList<int> m_fds;
};

} // namespace vela::files
