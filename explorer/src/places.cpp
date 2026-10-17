// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "places.h"

#include "foldermodel.h"
#include "links.h"
#include "transferlocks.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QXmlStreamReader>

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <utility>

using ManagedObjects = QMap<QDBusObjectPath, QMap<QString, QVariantMap>>;
Q_DECLARE_METATYPE(ManagedObjects)

namespace {

const QString udisks = QStringLiteral("org.freedesktop.UDisks2");

// udisks byte strings ("ay"), without the trailing zero.
QString bytes(const QVariant& value)
{
    QByteArray data = value.toByteArray();
    while (data.endsWith('\0')) {
        data.chop(1);
    }
    return QString::fromLocal8Bit(data);
}

bool hasMountPoints(const QVariant& value)
{
    if (value.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = value.value<QDBusArgument>();
        QList<QByteArray> points;
        arg >> points;
        return !points.isEmpty();
    }
    return !value.value<QList<QByteArray>>().isEmpty();
}

struct UserDir {
    QStandardPaths::StandardLocation location;
    const char* icon;
};

const UserDir userDirs[] = {
    { QStandardPaths::DesktopLocation, "user-desktop" },
    { QStandardPaths::DownloadLocation, "folder-download" },
    { QStandardPaths::DocumentsLocation, "folder-documents" },
    { QStandardPaths::PicturesLocation, "folder-pictures" },
    { QStandardPaths::MusicLocation, "folder-music" },
    { QStandardPaths::MoviesLocation, "folder-videos" },
};

} // namespace

Places::Places(QObject* parent)
    : QObject(parent)
{
    loadQuickAccess();
    refreshDrives();
}

QString Places::home() const
{
    return QDir::homePath();
}

QString Places::trash() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash/files");
}

QString Places::userName() const
{
    if (const passwd* pw = getpwuid(getuid())) {
        return QString::fromLocal8Bit(pw->pw_name);
    }
    return qEnvironmentVariable("USER");
}

QString Places::iconFor(const QString& path) const
{
    for (const UserDir& dir : userDirs) {
        if (QStandardPaths::writableLocation(dir.location) == path) {
            return QLatin1String(dir.icon);
        }
    }
    if (path == QDir::homePath()) {
        return QStringLiteral("user-home");
    }
    if (path == trash()) {
        return QStringLiteral("user-trash");
    }
    return QStringLiteral("folder");
}

QString Places::displayName(const QString& path) const
{
    if (path == QStringLiteral("/")) {
        return QCoreApplication::translate("Files", "Local Disk");
    }
    if (path == trash()) {
        return QCoreApplication::translate("Files", "Recycle Bin");
    }
    if (path == QDir::homePath()) {
        return userName();
    }
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? path : name;
}

void Places::loadQuickAccess()
{
    QSettings settings;
    if (settings.contains(QStringLiteral("quickAccess"))) {
        m_pinned = settings.value(QStringLiteral("quickAccess")).toStringList();
    } else {
        // The user's folders first, like Windows.
        for (const UserDir& dir : userDirs) {
            const QString path = QStandardPaths::writableLocation(dir.location);
            if (!path.isEmpty() && path != QDir::homePath() && !m_pinned.contains(path)) {
                m_pinned.append(path);
            }
        }
    }
    m_quickAccess.clear();
    for (const QString& path : std::as_const(m_pinned)) {
        if (!QFileInfo(path).isDir()) {
            continue;
        }
        m_quickAccess.append(QVariantMap { { QStringLiteral("name"), displayName(path) },
            { QStringLiteral("path"), path }, { QStringLiteral("icon"), iconFor(path) }, { QStringLiteral("pinned"), true } });
    }
    emit quickAccessChanged();
}

void Places::saveQuickAccess()
{
    QSettings().setValue(QStringLiteral("quickAccess"), m_pinned);
    loadQuickAccess();
}

bool Places::isPinned(const QString& path) const
{
    return m_pinned.contains(path);
}

void Places::pin(const QString& path)
{
    if (!m_pinned.contains(path)) {
        m_pinned.append(path);
        saveQuickAccess();
    }
}

void Places::unpin(const QString& path)
{
    if (m_pinned.removeAll(path) > 0) {
        saveQuickAccess();
    }
}

void Places::refreshDrives()
{
    // Real drives (a device in /dev), once even when mounted in several places
    // (btrfs subvolumes): the shortest mount point is kept.
    static const QStringList skipped { QStringLiteral("/boot"), QStringLiteral("/efi"), QStringLiteral("/boot/efi") };
    QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    std::sort(volumes.begin(), volumes.end(),
        [](const QStorageInfo& a, const QStorageInfo& b) { return a.rootPath().size() < b.rootPath().size(); });
    QVariantList drives;
    QStringList devices;
    for (const QStorageInfo& volume : std::as_const(volumes)) {
        const QString device = QString::fromLocal8Bit(volume.device());
        if (!volume.isValid() || !volume.isReady() || !device.startsWith(QLatin1String("/dev/"))
            || skipped.contains(volume.rootPath()) || devices.contains(device)) {
            continue;
        }
        devices.append(device);
        const QString root = volume.rootPath();
        const bool removable = root.startsWith(QLatin1String("/run/media/")) || root.startsWith(QLatin1String("/media/"));
        QString name = root == QLatin1String("/") ? QCoreApplication::translate("Files", "Local Disk") : volume.displayName();
        if (name == root) {
            name = QFileInfo(root).fileName();
        }
        drives.append(QVariantMap {
            { QStringLiteral("name"), name },
            { QStringLiteral("path"), root },
            { QStringLiteral("icon"), removable ? QStringLiteral("drive-removable-media") : QStringLiteral("drive-harddisk") },
            { QStringLiteral("total"), double(volume.bytesTotal()) },
            { QStringLiteral("free"), double(volume.bytesAvailable()) },
            { QStringLiteral("device"), device },
            { QStringLiteral("removable"), removable },
            { QStringLiteral("mounted"), true },
            { QStringLiteral("volume"), QString() },
        });
    }
    m_mountedDrives = drives;
    publishDrives();
    readVolumes();
}

void Places::publishDrives()
{
    QVariantList drives = m_mountedDrives;
    drives += m_volumes;
    if (drives != m_drives) {
        m_drives = drives;
        emit drivesChanged();
    }
}

void Places::readVolumes()
{
    if (m_readingVolumes) {
        return;
    }
    m_readingVolumes = true;
    static const bool registered = [] {
        qDBusRegisterMetaType<ManagedObjects>();
        return true;
    }();
    Q_UNUSED(registered);
    QDBusMessage call = QDBusMessage::createMethodCall(udisks, QStringLiteral("/org/freedesktop/UDisks2"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        watcher->deleteLater();
        m_readingVolumes = false;
        const QDBusPendingReply<ManagedObjects> reply = *watcher;
        if (reply.isError()) {
            return; // no udisks: only mounted drives
        }
        const ManagedObjects objects = reply.value();
        const QString blockIface = QStringLiteral("org.freedesktop.UDisks2.Block");
        const QString fsIface = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
        const QString driveIface = QStringLiteral("org.freedesktop.UDisks2.Drive");
        QVariantList volumes;
        m_devices.clear();
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const QVariantMap block = it.value().value(blockIface);
            if (block.isEmpty()) {
                continue;
            }
            const QString device = bytes(block.value(QStringLiteral("PreferredDevice")));
            const QString drivePath = qvariant_cast<QDBusObjectPath>(block.value(QStringLiteral("Drive"))).path();
            const QVariantMap drive = objects.value(QDBusObjectPath(drivePath)).value(driveIface);
            const bool removable = drive.value(QStringLiteral("Removable")).toBool()
                || drive.value(QStringLiteral("Ejectable")).toBool()
                || drive.value(QStringLiteral("ConnectionBus")).toString() == QLatin1String("usb");
            m_devices.insert(device,
                { { QStringLiteral("block"), it.key().path() }, { QStringLiteral("drive"), drivePath },
                    { QStringLiteral("removable"), removable } });
            m_devices.insert(bytes(block.value(QStringLiteral("Device"))), m_devices.value(device));
            // Unmounted ones that can be opened: real filesystems, not hidden
            // (the EFI partition, recovery ones), not swap.
            if (!it.value().contains(fsIface) || block.value(QStringLiteral("IdUsage")).toString() != QLatin1String("filesystem")
                || block.value(QStringLiteral("HintIgnore")).toBool()
                || hasMountPoints(it.value().value(fsIface).value(QStringLiteral("MountPoints")))) {
                continue;
            }
            const double size = block.value(QStringLiteral("Size")).toDouble();
            QString name = block.value(QStringLiteral("IdLabel")).toString();
            if (name.isEmpty()) {
                name = QCoreApplication::translate("Files", "Volume of ") + formatSize(qint64(size));
            }
            volumes.append(QVariantMap {
                { QStringLiteral("name"), name },
                { QStringLiteral("path"), QString() },
                { QStringLiteral("icon"), removable ? QStringLiteral("drive-removable-media") : QStringLiteral("drive-harddisk") },
                { QStringLiteral("total"), size },
                { QStringLiteral("free"), -1.0 },
                { QStringLiteral("device"), device },
                { QStringLiteral("removable"), removable },
                { QStringLiteral("mounted"), false },
                { QStringLiteral("volume"), it.key().path() },
            });
        }
        std::sort(volumes.begin(), volumes.end(), [](const QVariant& a, const QVariant& b) {
            return a.toMap().value(QStringLiteral("device")).toString() < b.toMap().value(QStringLiteral("device")).toString();
        });
        m_volumes = volumes;
        // Mounted removable ones know better from udisks.
        for (QVariant& entry : m_mountedDrives) {
            QVariantMap map = entry.toMap();
            const QVariantMap known = m_devices.value(map.value(QStringLiteral("device")).toString());
            if (!known.isEmpty() && known.value(QStringLiteral("removable")).toBool()) {
                map[QStringLiteral("removable")] = true;
                map[QStringLiteral("icon")] = QStringLiteral("drive-removable-media");
                entry = map;
            }
        }
        publishDrives();
        if (!m_watchingVolumes) {
            m_watchingVolumes = true;
            QDBusConnection system = QDBusConnection::systemBus();
            for (const char* signal : { "InterfacesAdded", "InterfacesRemoved" }) {
                system.connect(udisks, QStringLiteral("/org/freedesktop/UDisks2"),
                    QStringLiteral("org.freedesktop.DBus.ObjectManager"), QLatin1String(signal), this,
                    SLOT(onVolumesChanged()));
            }
            system.connect(udisks, QString(), QStringLiteral("org.freedesktop.DBus.Properties"),
                QStringLiteral("PropertiesChanged"), this, SLOT(onVolumesChanged()));
        }
    });
}

void Places::onVolumesChanged()
{
    // They come in bursts: a single pass.
    QTimer::singleShot(300, this, [this] { refreshDrives(); });
}

void Places::mount(const QString& volume)
{
    QDBusMessage call = QDBusMessage::createMethodCall(udisks, volume, QStringLiteral("org.freedesktop.UDisks2.Filesystem"),
        QStringLiteral("Mount"));
    call << QVariantMap { { QStringLiteral("auth.no_user_interaction"), false } };
    // The password (polkit) can take a while.
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 5 * 60 * 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, volume] {
        watcher->deleteLater();
        const QDBusPendingReply<QString> reply = *watcher;
        if (reply.isError()) {
            const QString name = reply.error().name();
            emit mounted(volume, {},
                name.endsWith(QLatin1String("NotAuthorizedDismissed")) ? QString()
                    : QCoreApplication::translate("Files", "Couldn't open the drive: ") + reply.error().message());
        } else {
            emit mounted(volume, reply.value(), {});
        }
        refreshDrives();
    });
}

void Places::setTransferGuard(std::function<bool(const QString&)> guard)
{
    m_transferGuard = std::move(guard);
}

void Places::eject(const QString& device)
{
    const QVariantMap known = m_devices.value(device);
    if (known.isEmpty()) {
        emit ejected(QCoreApplication::translate("Files", "Couldn't find the drive."));
        return;
    }
    // Eject powers off the entire drive, not merely the selected partition.
    // Both PreferredDevice and Device aliases are registered in m_devices.
    QSet<QString> related { device };
    const QString drive = known.value(QStringLiteral("drive")).toString();
    if (!drive.isEmpty() && drive != QLatin1String("/")) {
        for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
            if (it.value().value(QStringLiteral("drive")).toString() == drive) {
                related.insert(it.key());
            }
        }
    }
    for (const QString& name : std::as_const(related)) {
        if (m_ejecting.contains(name)) {
            emit ejected(QCoreApplication::translate("Files", "Drive ejection is already in progress."));
            return;
        }
        if (m_transferGuard && m_transferGuard(name)) {
            emit ejected(QCoreApplication::translate("Files",
                "A file transfer is using this drive. Wait for it to finish or cancel it before ejecting."));
            return;
        }
    }

    // Acquire all sibling filesystem locks *before* starting UDisks.
    // LOCK_EX conflicts with transfers in any vela-files process and stays
    // reserved until the asynchronous eject finishes.
    auto locks = std::make_shared<vela::files::DeviceLocks>();
    if (!locks->acquire(related.values(), vela::files::DeviceLocks::Mode::Exclusive)) {
        emit ejected(QCoreApplication::translate("Files",
            "A file transfer is using this drive. Wait for it to finish or cancel it before ejecting."));
        return;
    }
    m_ejecting.unite(related);
    const auto finish = [this, related, locks](const QString& error) {
        locks->release();
        for (const QString& name : related) {
            m_ejecting.remove(name);
        }
        emit ejected(error);
        refreshDrives();
    };
    QDBusMessage unmount = QDBusMessage::createMethodCall(udisks, known.value(QStringLiteral("block")).toString(),
        QStringLiteral("org.freedesktop.UDisks2.Filesystem"), QStringLiteral("Unmount"));
    unmount << QVariantMap {};
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(unmount), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, known, finish] {
        watcher->deleteLater();
        const QDBusPendingReply<> reply = *watcher;
        if (reply.isError()) {
            finish(reply.error().name().endsWith(QLatin1String("DeviceBusy"))
                    ? QCoreApplication::translate("Files", "The drive is in use: close any open files and try again.")
                    : QCoreApplication::translate("Files", "Couldn't eject the drive: ") + reply.error().message());
            return;
        }
        const QString drive = known.value(QStringLiteral("drive")).toString();
        if (drive.isEmpty() || drive == QLatin1String("/")) {
            finish({});
            return;
        }
        QDBusMessage eject = QDBusMessage::createMethodCall(udisks, drive,
            QStringLiteral("org.freedesktop.UDisks2.Drive"), QStringLiteral("Eject"));
        eject << QVariantMap {};
        auto* ejectWatcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(eject), this);
        connect(ejectWatcher, &QDBusPendingCallWatcher::finished, this, [ejectWatcher, finish] {
            ejectWatcher->deleteLater();
            // As before, an unsupported power-off does not undo a successful unmount.
            finish({});
        });
    });
}

// ------------------------------------------------------------ Favorites --

QVariantList Places::favorites() const
{
    static const QMimeDatabase mimes;
    QVariantList out;
    for (const QString& path : favoriteFiles()) {
        const QFileInfo info(path);
        if (!info.exists()) {
            continue;
        }
        out.append(QVariantMap {
            { QStringLiteral("name"), info.fileName() },
            { QStringLiteral("path"), path },
            { QStringLiteral("icon"), info.isDir() ? QStringLiteral("folder") : mimes.mimeTypeForFile(info, QMimeDatabase::MatchExtension).iconName() },
            { QStringLiteral("modified"), info.lastModified() },
            { QStringLiteral("location"), info.absolutePath() },
        });
    }
    return out;
}

bool Places::isFavorite(const QString& path) const
{
    return ::isFavorite(path);
}

void Places::setFavorite(const QString& path, bool on)
{
    ::setFavorite(path, on);
    emit favoritesChanged();
}



QVariantList Places::recentFiles(int limit) const
{
    // <bookmark href="file:///..." modified="2026-10-04T12:00:00Z">
    struct Recent {
        QString path;
        QDateTime modified;
    };
    QList<Recent> all;
    QFile file(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/recently-used.xbel"));
    if (file.open(QIODevice::ReadOnly)) {
        QXmlStreamReader xml(&file);
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("bookmark")) {
                const QUrl url(xml.attributes().value(QLatin1String("href")).toString());
                if (url.isLocalFile()) {
                    all.append({ url.toLocalFile(),
                        QDateTime::fromString(xml.attributes().value(QLatin1String("modified")).toString(), Qt::ISODate) });
                }
            }
        }
    }
    std::sort(all.begin(), all.end(), [](const Recent& a, const Recent& b) { return a.modified > b.modified; });
    static const QMimeDatabase mimes;
    QVariantList out;
    for (const Recent& r : std::as_const(all)) {
        const QFileInfo info(r.path);
        if (!info.exists() || info.isDir()) {
            continue;
        }
        const QMimeType mime = mimes.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
        out.append(QVariantMap {
            { QStringLiteral("name"), info.fileName() },
            { QStringLiteral("path"), r.path },
            { QStringLiteral("icon"), mime.iconName() },
            { QStringLiteral("modified"), r.modified.toLocalTime() },
            { QStringLiteral("location"), info.absolutePath() },
        });
        if (out.size() >= limit) {
            break;
        }
    }
    return out;
}
