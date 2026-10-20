// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fileops.h"

#include "appmodel.h"
#include "foldermodel.h"
#include "links.h"
#include "peek.h"
#include "systemactions.h"
#include "transferlocks.h"

#include <QClipboard>
#include <QCollator>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QScreen>
#include <QStandardPaths>
#include <QWindow>
#include <QThreadPool>
#include <QStorageInfo>
#include <QAbstractListModel>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <utility>

namespace {

QString trashDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash");
}

bool sameDevice(const QString& a, const QString& b)
{
    struct stat sa {};
    struct stat sb {};
    return ::lstat(QFile::encodeName(a).constData(), &sa) == 0 && ::stat(QFile::encodeName(b).constData(), &sb) == 0
        && sa.st_dev == sb.st_dev;
}

bool runDetached(const QString& program, const QStringList& arguments, const QString& directory)
{
    return QProcess::startDetached(program, arguments, directory);
}

// "photo.jpg" -> "photo - Copy.jpg", like Windows when pasting into the same
// folder.
QString copyName(const QFileInfo& source)
{
    const bool hasSuffix = !source.isDir() && !source.suffix().isEmpty() && !source.completeBaseName().isEmpty();
    return hasSuffix ? source.completeBaseName() + QCoreApplication::translate("Files", " - Copy.") + source.suffix()
                     : source.fileName() + QCoreApplication::translate("Files", " - Copy");
}

// A hidden free name next to `destination`, on the same disk: the copy is
// written there and published atomically only once complete.
// "photo.jpg" -> ".photo.jpg.vela-copy-XXXXXX". With `directory` it
// creates an empty folder, otherwise an empty file (0600).
QString makeTemporary(const QString& destination, bool directory)
{
    const QFileInfo info(destination);
    QByteArray name = QFile::encodeName(info.fileName());
    name.truncate(200); // the whole name must not exceed NAME_MAX
    QByteArray pattern = QFile::encodeName(info.absolutePath()) + "/." + name + ".vela-copy-XXXXXX";
    if (directory) {
        return ::mkdtemp(pattern.data()) ? QFile::decodeName(pattern) : QString();
    }
    const int fd = ::mkostemp(pattern.data(), O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    ::close(fd);
    return QFile::decodeName(pattern);
}

bool removeAny(const QString& path)
{
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
}

// Writes to disk everything pending on `directory`'s filesystem.
void syncDirectory(const QString& directory)
{
    const int fd = ::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::syncfs(fd);
        ::close(fd);
    }
}

bool renameOver(const QString& from, const QString& to)
{
    return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
}

bool exchangeWith(const QString& from, const QString& to)
{
    // Unlike rename(), this can replace a nonempty directory with a file (or
    // vice versa). The old destination becomes `from` in the same operation:
    // there is never a moment when `to` is missing. Unsupported filesystems
    // must fail safely; never fall back to deleting `to` before a rename.
    return ::renameat2(AT_FDCWD, QFile::encodeName(from).constData(),
               AT_FDCWD, QFile::encodeName(to).constData(), RENAME_EXCHANGE)
        == 0;
}

void sendToShell(const QByteArray& command)
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    QLocalSocket socket;
    socket.connectToServer(runtimeDir + QStringLiteral("/vela-shell-") + display + QStringLiteral(".sock"));
    if (socket.waitForConnected(300)) {
        socket.write(command + '\n');
        socket.waitForBytesWritten(300);
    }
}

} // namespace

QString uniqueNameIn(const QString& directory, const QString& name)
{
    const QDir dir(directory);
    if (!QFileInfo::exists(dir.filePath(name))) {
        return name;
    }
    const QFileInfo info(name);
    const bool hasSuffix = !info.suffix().isEmpty() && !info.completeBaseName().isEmpty()
        && !QFileInfo(dir.filePath(name)).isDir();
    const QString base = hasSuffix ? info.completeBaseName() : name;
    const QString suffix = hasSuffix ? u'.' + info.suffix() : QString();
    for (int n = 2;; ++n) {
        const QString candidate = base + QStringLiteral(" (%1)").arg(n) + suffix;
        if (!QFileInfo::exists(dir.filePath(candidate))) {
            return candidate;
        }
    }
}

// ------------------------------------------------------------- long jobs --

struct FileOps::Job {
    int id = 0;
    QString title;
    QString doneTitle; // when the transfer is over
    qint64 done = 0;
    qint64 total = 0;
    QString current;
    bool finished = false;
    QString error;
    QStringList sources;
    QString destination;
    std::shared_ptr<vela::files::DeviceLocks> deviceLocks;
    std::atomic<bool> cancelled { false };
};

// A stable item model: ordinary progress emits dataChanged for one row,
// rather than replacing the ListView's whole JavaScript array every 100 ms.
class TransferJobsModel final : public QAbstractListModel {
public:
    explicit TransferJobsModel(QObject* parent) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()
            || role != Qt::UserRole + 1) {
            return {};
        }
        return m_rows.at(index.row());
    }
    QHash<int, QByteArray> roleNames() const override
    {
        return { { Qt::UserRole + 1, QByteArrayLiteral("modelData") } };
    }

    void add(const QVariantMap& job)
    {
        // The newest copy must be visible even if several other jobs fill
        // the viewport; completed and failed cards remain below active jobs.
        beginInsertRows({}, 0, 0);
        m_rows.prepend(job);
        endInsertRows();
    }
    void update(int id, const QVariantMap& job)
    {
        for (int row = 0; row < m_rows.size(); ++row) {
            if (m_rows.at(row).value(QStringLiteral("id")).toInt() != id) {
                continue;
            }
            if (job.value(QStringLiteral("finished")).toBool()
                && !m_rows.at(row).value(QStringLiteral("finished")).toBool()
                && row != m_rows.size() - 1) {
                const int last = int(m_rows.size());
                beginMoveRows({}, row, row, {}, last);
                m_rows.move(row, last - 1);
                endMoveRows();
                row = last - 1;
            }
            m_rows[row] = job;
            emit dataChanged(index(row), index(row), { Qt::UserRole + 1 });
            return;
        }
    }
    void dismiss(int id)
    {
        for (int row = 0; row < m_rows.size(); ++row) {
            if (m_rows.at(row).value(QStringLiteral("id")).toInt() != id
                || !m_rows.at(row).value(QStringLiteral("finished")).toBool()) {
                continue;
            }
            beginRemoveRows({}, row, row);
            m_rows.removeAt(row);
            endRemoveRows();
            return;
        }
    }

private:
    QList<QVariantMap> m_rows;
};

FileOps::FileOps(AppModel* apps, QObject* parent)
    : QObject(parent)
    , m_apps(apps)
    , m_jobModel(new TransferJobsModel(this))
{
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &FileOps::clipboardChanged);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        bus.registerObject(QStringLiteral("/org/vela/Files/Transfers"), this,
            QDBusConnection::ExportScriptableSlots);
        bus.connect(QString(), QStringLiteral("/org/vela/Files/Transfers"),
            QStringLiteral("org.vela.Files.Transfers"), QStringLiteral("RequestSnapshot"),
            this, SLOT(publishSnapshot()));
    }
}

FileOps::~FileOps()
{
    for (const auto& job : std::as_const(m_jobs)) {
        job->cancelled = true;
    }
}

QVariantMap FileOps::jobAsMap(const Job& job) const
{
    return {
        { QStringLiteral("id"), job.id },
        { QStringLiteral("title"), job.title },
        { QStringLiteral("doneTitle"), job.doneTitle },
        { QStringLiteral("done"), double(job.done) },
        { QStringLiteral("total"), double(job.total) },
        { QStringLiteral("current"), job.current },
        { QStringLiteral("finished"), job.finished },
        { QStringLiteral("error"), job.error },
    };
}

QVariantList FileOps::jobs() const
{
    QVariantList out;
    for (const auto& job : m_jobs) {
        out.append(jobAsMap(*job));
    }
    return out;
}

QAbstractItemModel* FileOps::jobModel() const
{
    return m_jobModel;
}

int FileOps::activeJobCount() const
{
    return int(std::count_if(m_jobs.cbegin(), m_jobs.cend(),
        [](const std::shared_ptr<Job>& job) { return !job->finished; }));
}

bool FileOps::pathsOnDevice(const QStringList& sources, const QString& destination, const QString& device)
{
    if (device.isEmpty()) {
        return false;
    }
    return vela::files::transferDevices(sources, destination).contains(vela::files::canonicalDevice(device));
}

bool FileOps::activeOnDevice(const QString& device) const
{
    for (const auto& job : m_jobs) {
        if (!job->finished && pathsOnDevice(job->sources, job->destination, device)) {
            return true;
        }
    }
    return false;
}

void FileOps::setEjectGuard(std::function<bool(const QStringList&, const QString&)> guard)
{
    m_ejectGuard = std::move(guard);
}

void FileOps::publishJob(int id, bool snapshot) const
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    for (const auto& job : m_jobs) {
        if (job->id != id) continue;
        QDBusMessage msg = QDBusMessage::createSignal(QStringLiteral("/org/vela/Files/Transfers"),
            QStringLiteral("org.vela.Files.Transfers"),
            snapshot ? QStringLiteral("Snapshot") : QStringLiteral("Changed"));
        msg << bus.baseService() << job->id << job->title << qlonglong(job->done)
            << qlonglong(job->total) << job->current << job->finished << job->error;
        bus.send(msg);
        break;
    }
}

void FileOps::publishRemoved(int id) const
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    QDBusMessage msg = QDBusMessage::createSignal(QStringLiteral("/org/vela/Files/Transfers"),
        QStringLiteral("org.vela.Files.Transfers"), QStringLiteral("Removed"));
    msg << bus.baseService() << id;
    bus.send(msg);
}

void FileOps::publishSnapshot()
{
    for (const auto& job : m_jobs) publishJob(job->id, true);
}

void FileOps::Cancel(int id)
{
    cancelJob(id);
}

void FileOps::updateJob(int id, qint64 done, qint64 total, const QString& current, bool finished, const QString& error)
{
    for (const auto& job : std::as_const(m_jobs)) {
        if (job->id == id) {
            job->done = done;
            job->total = total;
            job->current = current;
            job->finished = finished;
            job->error = error;
            m_jobModel->update(id, jobAsMap(*job));
            break;
        }
    }
    emit jobsChanged();
    publishJob(id);
}

void FileOps::cancelJob(int id)
{
    for (const auto& job : std::as_const(m_jobs)) {
        if (job->id == id) {
            job->cancelled = true;
        }
    }
}

void FileOps::dismissJob(int id)
{
    const qsizetype before = m_jobs.size();
    m_jobs.removeIf([id](const std::shared_ptr<Job>& job) { return job->id == id && job->finished; });
    if (m_jobs.size() != before) {
        m_jobModel->dismiss(id);
        publishRemoved(id);
        emit jobsChanged();
    }
}

// Before copying or moving: is there already something with the same name?
void FileOps::requestTransfer(const Transfer& transfer)
{
    int conflicts = 0;
    QString first;
    for (const QString& source : transfer.sources) {
        const QFileInfo info(source);
        if (info.absolutePath() == QDir(transfer.directory).absolutePath()) {
            continue; // in the same folder: "- Copy" (or nothing, when moving)
        }
        if (QFileInfo::exists(QDir(transfer.directory).filePath(info.fileName()))) {
            if (++conflicts == 1) {
                first = info.fileName();
            }
        }
    }
    if (conflicts > 0) {
        m_pending = transfer;
        emit conflict(conflicts, first, transfer.directory);
        return;
    }
    startTransfer(transfer, QStringLiteral("keep"));
}

void FileOps::resolveConflict(const QString& choice)
{
    const Transfer transfer = std::exchange(m_pending, {});
    if (choice != QLatin1String("cancel") && !transfer.sources.isEmpty()) {
        startTransfer(transfer, choice);
    }
}

// Work units use the same rules as copyOne: each directory and symlink
// contributes one unit, nonempty regular files contribute their byte size.
qint64 transferWorkUnits(const QString& source)
{
    const QFileInfo info(source);
    if (info.isSymLink()) {
        return 1;
    }
    if (!info.isDir()) {
        return std::max<qint64>(1, info.size());
    }
    qint64 total = 1;
    QDirIterator it(source, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo entry = it.fileInfo();
        total += entry.isSymLink() || entry.isDir() ? 1 : std::max<qint64>(1, entry.size());
    }
    return total;
}

void FileOps::startTransfer(const Transfer& transfer, const QString& policy)
{
    if (m_ejectGuard && m_ejectGuard(transfer.sources, transfer.directory)) {
        emit failed(QCoreApplication::translate("Files",
            "This drive is being ejected. Wait for ejection to finish before transferring files."));
        return;
    }
    // The lock must be held before the job becomes visible, and through the
    // worker's final sync. Eject in another Explorer process takes LOCK_EX.
    auto locks = std::make_shared<vela::files::DeviceLocks>();
    if (!locks->acquire(vela::files::transferDevices(transfer.sources, transfer.directory),
            vela::files::DeviceLocks::Mode::Shared)) {
        emit failed(QCoreApplication::translate("Files",
            "A drive is being ejected or the transfer lock is unavailable. Try again."));
        return;
    }
    auto job = std::make_shared<Job>();
    job->deviceLocks = std::move(locks);
    job->id = m_nextJob++;
    job->sources = transfer.sources;
    job->destination = transfer.directory;
    const int count = int(transfer.sources.size());
    const QString where = QFileInfo(transfer.directory).fileName().isEmpty() ? transfer.directory
                                                                              : QFileInfo(transfer.directory).fileName();
    // The title sentence by sentence, so each language puts the parts in
    // order.
    const QString items = count == 1 ? QCoreApplication::translate("Files", "1 item")
                                     : QCoreApplication::translate("Files", "%1 items").arg(count);
    job->title = (transfer.move ? QCoreApplication::translate("Files", "Moving %1 to %2")
                                : QCoreApplication::translate("Files", "Copying %1 to %2"))
                     .arg(items, where);
    job->doneTitle = QCoreApplication::translate("Files", "Done: %1 to %2").arg(items, where);
    m_jobs.append(job);
    m_jobModel->add(jobAsMap(*job));
    emit jobsChanged();
    publishJob(job->id);

    QPointer<FileOps> self(this);
    auto post = [self, id = job->id](qint64 done, qint64 total, QString current, bool finished, QString error) {
        QMetaObject::invokeMethod(
            qApp,
            [=] {
                if (self) {
                    self->updateJob(id, done, total, current, finished, error);
                }
            },
            Qt::QueuedConnection);
    };
    QThreadPool::globalInstance()->start([self, job, transfer, policy, post] {
        // Count every directory and symlink as one unit, and files by bytes.
        // This exactly matches copyOne(), including empty nested directories.
        // A same-device move may fall back to copying if rename fails.
        qint64 total = 0;
        QSet<QString> estimatedRenames;
        for (const QString& source : transfer.sources) {
            if (transfer.move && sameDevice(source, transfer.directory)) {
                // Common same-filesystem moves are O(1) renames, not tree walks.
                estimatedRenames.insert(source);
                total += 1;
            } else {
                total += transferWorkUnits(source);
            }
        }
        qint64 done = 0;
        QElapsedTimer lastPost;
        lastPost.start();
        QString error;
        UndoStep step;
        step.label = transfer.move ? QCoreApplication::translate("Files", "Move") : QCoreApplication::translate("Files", "Copy");
        QString firstArrived;

        // Copies `from` to `to` without ever leaving `to` half-written. A file
        // is written to a hidden temporary next to `to` and becomes `to` with
        // an atomic rename only once the copy is done: if a file with that
        // name existed, it stays intact until that moment. Errors, a full disk
        // and cancelling remove the temporary. Folders are created (or, if
        // they exist, merged) and copied file by file.
        std::function<bool(const QString&, const QString&)> copyOne = [&](const QString& from, const QString& to) {
            const QFileInfo info(from);
            if (job->cancelled) {
                return false;
            }
            if (info.isSymLink()) {
                const QString temporary = makeTemporary(to, false);
                QFile::remove(temporary); // the link in its place
                // The target as written: a relative link stays relative and
                // points to the file next to it, in the copy.
                if (temporary.isEmpty() || !QFile::link(info.readSymLink(), temporary) || !renameOver(temporary, to)) {
                    QFile::remove(temporary);
                    error = QCoreApplication::translate("Files", "Couldn't create the link %1").arg(to);
                    return false;
                }
                done += 1;
                return true;
            }
            if (info.isDir()) {
                if (!QDir().mkpath(to)) {
                    error = QCoreApplication::translate("Files", "Couldn't create the folder %1").arg(to);
                    return false;
                }
                const QFileInfoList children = QDir(from).entryInfoList(
                    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
                for (const QFileInfo& child : children) {
                    if (!copyOne(child.absoluteFilePath(), QDir(to).filePath(child.fileName()))) {
                        return false;
                    }
                }
                QFile::setPermissions(to, info.permissions());
                done += 1;
                return true;
            }
            QFile in(from);
            if (!in.open(QIODevice::ReadOnly)) {
                error = QCoreApplication::translate("Files", "Couldn't read %1").arg(info.fileName());
                return false;
            }
            const QString temporary = makeTemporary(to, false);
            QFile out(temporary);
            if (temporary.isEmpty() || !out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QFile::remove(temporary);
                error = QCoreApplication::translate("Files", "Couldn't write to %1").arg(QFileInfo(to).absolutePath());
                return false;
            }
            const auto fail = [&](const QString& message) {
                out.close();
                QFile::remove(temporary);
                error = message;
                return false;
            };
            while (!in.atEnd()) {
                if (job->cancelled) {
                    out.close();
                    QFile::remove(temporary);
                    return false;
                }
                const QByteArray buffer = in.read(4 << 20);
                if (buffer.isEmpty() && in.error() != QFile::NoError) {
                    return fail(QCoreApplication::translate("Files", "Error reading %1").arg(info.fileName()));
                }
                if (out.write(buffer) != buffer.size()) {
                    return fail(QCoreApplication::translate("Files", "Out of space or error writing %1").arg(info.fileName()));
                }
                done += buffer.size();
                if (lastPost.elapsed() > 100) {
                    post(done, total, info.fileName(), false, QString());
                    lastPost.restart();
                }
            }
            if (!out.flush()) {
                return fail(QCoreApplication::translate("Files", "Out of space or error writing %1").arg(info.fileName()));
            }
            // Attributes after the last write (which would change the date)
            // and before the rename: whoever sees `to` sees it complete.
            out.setPermissions(info.permissions());
            out.setFileTime(info.lastModified(), QFileDevice::FileModificationTime);
            // Replacing an existing file: the new data on disk before the old
            // one disappears (syncfs writes the rest at the end).
            if (QFileInfo::exists(to) && ::fdatasync(out.handle()) != 0) {
                return fail(QCoreApplication::translate("Files", "Out of space or error writing %1").arg(info.fileName()));
            }
            out.close();
            if (!renameOver(temporary, to)) {
                QFile::remove(temporary);
                error = QCoreApplication::translate("Files", "Couldn't write %1").arg(to);
                return false;
            }
            if (info.size() == 0) {
                done += 1;
            }
            return true;
        };

        for (const QString& source : transfer.sources) {
            if (job->cancelled || !error.isEmpty()) {
                break;
            }
            const QFileInfo info(source);
            const QDir target(transfer.directory);
            const bool sameDir = info.absolutePath() == target.absolutePath();
            if (sameDir && transfer.move) {
                continue;
            }
            if (transfer.directory == source || transfer.directory.startsWith(source + u'/')) {
                error = QCoreApplication::translate("Files", "The destination folder is inside the source folder.");
                break;
            }
            QString name = info.fileName();
            bool replacing = false;
            if (sameDir) {
                name = uniqueNameIn(transfer.directory, copyName(info));
            } else if (QFileInfo::exists(target.filePath(name)) || QFileInfo(target.filePath(name)).isSymLink()) {
                if (policy == QLatin1String("skip")) {
                    continue;
                }
                if (policy == QLatin1String("keep")) {
                    name = uniqueNameIn(transfer.directory, name);
                } else {
                    // Replace: the old one stays until the new one is ready
                    // (two folders are merged instead).
                    replacing = true;
                }
            }
            const QString destination = target.filePath(name);
            const QFileInfo existing(destination);
            const bool sourceIsDir = info.isDir() && !info.isSymLink();
            const bool merging = replacing && sourceIsDir && existing.isDir() && !existing.isSymLink();
            post(done, total, info.fileName(), false, QString());

            // Move on the same disk: a rename, which replaces an existing file
            // atomically (folders and different types don't: they go through
            // the copy).
            const bool sameKind = !existing.exists() || (!existing.isDir() && !sourceIsDir);
            if (transfer.move && !merging && sameKind && sameDevice(source, transfer.directory)
                && renameOver(source, destination)) {
                done += 1;
                step.moves.append({ destination, source });
                if (firstArrived.isEmpty()) {
                    firstArrived = destination;
                }
                continue;
            }

            // A failed/unavailable rename must use full copy accounting.
            if (estimatedRenames.contains(source)) {
                total += transferWorkUnits(source) - 1;
                post(done, total, info.fileName(), false, QString());
            }
            bool copied = false;
            if (merging || (!sourceIsDir && (!replacing || sameKind))) {
                // Merging folders, or a file (also replacing another file):
                // atomic file by file.
                copied = copyOne(source, destination);
            } else {
                // A new folder, or one type replacing another: prepare the
                // entire copy next to the destination before publishing it.
                const QString temporary = makeTemporary(destination, sourceIsDir);
                if (temporary.isEmpty()) {
                    error = QCoreApplication::translate("Files", "Couldn't write to %1").arg(transfer.directory);
                } else {
                    copied = copyOne(source, temporary);
                    if (copied && replacing) {
                        // Write the staged tree before exchanging the names.
                        syncDirectory(transfer.directory);
                    }
                    copied = copied && !job->cancelled;
                    if (copied) {
                        if (replacing) {
                            copied = exchangeWith(temporary, destination);
                            if (copied) {
                                // `temporary` now holds the old destination.
                                // Persist the exchange before removing it. A
                                // process killed here leaves both items intact.
                                syncDirectory(transfer.directory);
                                if (!removeAny(temporary)) {
                                    error = QCoreApplication::translate("Files", "Replaced %1, but couldn't remove the old item at %2")
                                                .arg(destination, temporary);
                                }
                            }
                        } else {
                            copied = renameOver(temporary, destination);
                        }
                    }
                    if (!copied && !removeAny(temporary)) {
                        if (!error.isEmpty()) {
                            error += u'\n';
                        }
                        error += QCoreApplication::translate("Files", "Couldn't remove the temporary item %1").arg(temporary);
                    }
                }
                if (!copied && error.isEmpty() && !job->cancelled) {
                    error = QCoreApplication::translate("Files", "Couldn't write %1").arg(destination);
                }
            }
            if (!copied) {
                break;
            }
            if (transfer.move) {
                // Moves that needed a copy: delete the source only when the
                // copy is on disk and cleanup has succeeded.
                if (error.isEmpty()) {
                    syncDirectory(transfer.directory);
                    if (removeAny(source)) {
                        step.moves.append({ destination, source });
                    } else {
                        error = QCoreApplication::translate("Files", "Copied %1, but couldn't remove the source %2")
                                    .arg(destination, source);
                    }
                }
            } else if (!merging) {
                step.created.append(destination);
            }
            if (firstArrived.isEmpty()) {
                firstArrived = destination;
            }
        }
        // Finish new copies with a filesystem sync. Type replacements have
        // already been synced before cleaning up the old destination.
        if (!step.created.isEmpty()) {
            syncDirectory(transfer.directory);
        }
        // No filesystem work remains: release shared locks before the UI
        // publishes completion. Retained result cards must not block eject.
        job->deviceLocks.reset();
        const bool cancelled = job->cancelled;
        const QString result = error.isEmpty() && cancelled ? QCoreApplication::translate("Files", "Canceled") : error;
        // Preserve progress on cancellation/failure; never claim 100%.
        post(error.isEmpty() && !cancelled ? total : done, total, QString(), true, result);
        QMetaObject::invokeMethod(
            qApp,
            [self, step, firstArrived, fromDesktop = transfer.fromDesktop, id = job->id, ok = error.isEmpty() && !cancelled] {
                if (!self) {
                    return;
                }
                if (!step.moves.isEmpty() || !step.created.isEmpty()) {
                    self->pushUndo(step);
                    if (fromDesktop) {
                        QJsonArray moves;
                        for (const auto& [current, original] : step.moves) {
                            moves.append(QJsonArray { current, original });
                        }
                        QJsonObject payload {
                            { QStringLiteral("label"), step.label },
                            { QStringLiteral("moves"), moves },
                            { QStringLiteral("created"), QJsonArray::fromStringList(step.created) }
                        };
                        // Report only committed operations, including the
                        // successful subset of a later failed transfer.
                        sendToShell("desktop-transfer-undo " + QJsonDocument(payload).toJson(QJsonDocument::Compact));
                    }
                }
                if (!firstArrived.isEmpty()) {
                    emit self->created(firstArrived, false);
                }
                // Finished fine: the panel disappears by itself after a
                // moment.
                if (ok) {
                    QTimer::singleShot(1500, self, [self, id] {
                        if (self) {
                            self->dismissJob(id);
                        }
                    });
                }
            },
            Qt::QueuedConnection);
    });
}

// ------------------------------------------------------------ opening --

namespace {

// A Linux program (ELF, AppImages included), not a library: it starts with a
// double click, like an .exe on Windows. Scripts stay documents: they open
// with the app for their type.
bool isProgram(const QFileInfo& info)
{
    if (!info.isFile() || info.fileName().contains(QLatin1String(".so"))) {
        return false;
    }
    QFile file(info.absoluteFilePath());
    return file.open(QIODevice::ReadOnly) && file.read(4) == QByteArray("\x7f" "ELF", 4);
}

} // namespace

void FileOps::open(const QStringList& paths)
{
    static const QMimeDatabase mimes;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (path.endsWith(QLatin1String(".desktop")) && m_apps->launchDesktopFile(path)) {
            continue;
        }
        if (isProgram(info)) {
            if (!info.isExecutable()) {
                emit failed(QCoreApplication::translate("Files", "“%1” is a program, but it can't be run. In Properties, turn on “Allow executing as a program”.")
                                .arg(info.fileName()));
            } else if (!runDetached(info.absoluteFilePath(), {}, info.absolutePath())) {
                emit failed(QCoreApplication::translate("Files", "Couldn't start “%1”.").arg(info.fileName()));
            }
            continue;
        }
        const QString app = defaultAppFor(mimes.mimeTypeForFile(info).name());
        if (app.isEmpty() || !m_apps->launchWithFile(app, QUrl::fromLocalFile(path).toString())) {
            chooseApp(path);
        }
    }
}

void FileOps::openWith(const QString& appId, const QString& path)
{
    m_apps->launchWithFile(appId, QUrl::fromLocalFile(path).toString());
}

QVariantList FileOps::appsFor(const QString& path) const
{
    return m_apps->appsForFile(path);
}

// ----------------------------------------------------------- clipboard --

namespace {

void putFiles(const QStringList& paths, bool cut)
{
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString& path : paths) {
        const QUrl url = QUrl::fromLocalFile(path);
        urls.append(url);
        gnome += '\n' + url.toEncoded();
    }
    if (urls.isEmpty()) {
        return;
    }
    auto* mime = new QMimeData;
    mime->setUrls(urls);
    // Like Dolphin and Nautilus: they say whether it's "cut" or "copy".
    mime->setData(QStringLiteral("application/x-kde-cutselection"), cut ? "1" : "0");
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnome);
    QGuiApplication::clipboard()->setMimeData(mime);
}

bool clipboardIsCut(const QMimeData* mime)
{
    return mime->data(QStringLiteral("application/x-kde-cutselection")) == "1"
        || mime->data(QStringLiteral("x-special/gnome-copied-files")).startsWith("cut");
}

} // namespace

void FileOps::copy(const QStringList& paths)
{
    putFiles(paths, false);
}

void FileOps::cut(const QStringList& paths)
{
    putFiles(paths, true);
}

bool FileOps::canPaste() const
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) {
        return false;
    }
    const QList<QUrl> urls = mime->urls();
    return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); });
}

QSet<QString> FileOps::cutPaths() const
{
    QSet<QString> out;
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime && mime->hasUrls() && clipboardIsCut(mime)) {
        for (const QUrl& url : mime->urls()) {
            if (url.isLocalFile()) {
                out.insert(url.toLocalFile());
            }
        }
    }
    return out;
}

void FileOps::paste(const QString& directory)
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) {
        return;
    }
    Transfer transfer;
    transfer.directory = directory;
    transfer.move = clipboardIsCut(mime);
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile())) {
            transfer.sources.append(QFileInfo(url.toLocalFile()).absoluteFilePath());
        }
    }
    if (transfer.sources.isEmpty()) {
        return;
    }
    if (transfer.move) {
        QGuiApplication::clipboard()->clear(); // moved: they don't paste twice
    }
    requestTransfer(transfer);
}

void FileOps::copyAsPath(const QStringList& paths)
{
    QStringList quoted;
    for (const QString& path : paths) {
        quoted.append(u'"' + path + u'"');
    }
    QGuiApplication::clipboard()->setText(quoted.join(u'\n'));
}

void FileOps::drop(const QStringList& urls, const QString& directory, int action)
{
    dropInternal(urls, directory, action, false);
}

void FileOps::dropFromDesktop(const QStringList& urls, const QString& directory, int action)
{
    dropInternal(urls, directory, action, true);
}

void FileOps::dropInternal(const QStringList& urls, const QString& directory, int action, bool fromDesktop)
{
    QStringList paths;
    for (const QString& text : urls) {
        const QUrl url(text);
        if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile())) {
            paths.append(QFileInfo(url.toLocalFile()).absoluteFilePath());
        }
    }
    if (paths.isEmpty()) {
        return;
    }
    if (directory == QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash/files")) {
        trash(paths);
        return;
    }
    Transfer transfer;
    transfer.directory = directory;
    transfer.fromDesktop = fromDesktop;
    transfer.move = action == 2 || (action == 0 && sameDevice(paths.first(), directory));
    for (const QString& path : std::as_const(paths)) {
        // Dropped in the folder they're already in: nothing to do.
        if (QFileInfo(path).absolutePath() != QDir(directory).absolutePath() || !transfer.move) {
            transfer.sources.append(path);
        }
    }
    if (!transfer.sources.isEmpty()) {
        requestTransfer(transfer);
    }
}

// ----------------------------------------------------------------- new --

QString FileOps::createFolder(const QString& directory)
{
    const QString name = uniqueNameIn(directory, QCoreApplication::translate("Files", "New folder"));
    if (!QDir(directory).mkdir(name)) {
        emit failed(QCoreApplication::translate("Files", "Couldn't create a folder here."));
        return {};
    }
    const QString path = QDir(directory).filePath(name);
    UndoStep step;
    step.label = QCoreApplication::translate("Files", "New");
    step.created.append(path);
    pushUndo(step);
    emit created(path, true);
    return path;
}

QString FileOps::createFile(const QString& directory, const QString& templatePath)
{
    const QString name = uniqueNameIn(directory,
        templatePath.isEmpty() ? QCoreApplication::translate("Files", "New Text Document.txt") : QFileInfo(templatePath).fileName());
    const QString path = QDir(directory).filePath(name);
    bool ok = false;
    if (templatePath.isEmpty()) {
        QFile file(path);
        ok = file.open(QIODevice::WriteOnly);
    } else {
        ok = QFile::copy(templatePath, path);
    }
    if (!ok) {
        emit failed(QCoreApplication::translate("Files", "Couldn't create a file here."));
        return {};
    }
    UndoStep step;
    step.label = QCoreApplication::translate("Files", "New");
    step.created.append(path);
    pushUndo(step);
    emit created(path, true);
    return path;
}

QVariantList FileOps::templates() const
{
    QVariantList out;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TemplatesLocation);
    if (dir.isEmpty() || QDir(dir) == QDir::home()) {
        return out;
    }
    static const QMimeDatabase mimes;
    for (const QFileInfo& info : QDir(dir).entryInfoList(QDir::Files, QDir::Name)) {
        out.append(QVariantMap { { QStringLiteral("name"), info.completeBaseName() },
            { QStringLiteral("icon"), mimes.mimeTypeForFile(info).iconName() },
            { QStringLiteral("path"), info.absoluteFilePath() } });
    }
    return out;
}

QString FileOps::rename(const QString& path, const QString& newName)
{
    const QString clean = newName.trimmed();
    const QFileInfo info(path);
    if (clean.isEmpty() || clean == info.fileName()) {
        return info.fileName();
    }
    if (clean.contains(u'/') || clean == QLatin1String(".") || clean == QLatin1String("..")) {
        return QCoreApplication::translate("Files", "!A name can't contain the / character");
    }
    const QString target = info.absoluteDir().filePath(clean);
    if (QFileInfo::exists(target)) {
        return QCoreApplication::translate("Files", "!This folder already contains an item named \"%1\".").arg(clean);
    }
    if (!QFile::rename(path, target)) {
        return QCoreApplication::translate("Files", "!Couldn't rename \"%1\".").arg(info.fileName());
    }
    UndoStep step;
    step.label = QCoreApplication::translate("Files", "Rename");
    step.moves.append({ target, path });
    pushUndo(step);
    return clean;
}

// --------------------------------------------------------- Recycle Bin --

void FileOps::trash(const QStringList& paths)
{
    UndoStep step;
    step.label = QCoreApplication::translate("Files", "Delete");
    for (const QString& path : paths) {
        QString inTrash;
        if (QFile::moveToTrash(path, &inTrash)) {
            step.trashed.append(inTrash);
            step.originals.append(path);
        } else {
            emit failed(QCoreApplication::translate("Files", "Couldn't move \"%1\" to the Recycle Bin.").arg(QFileInfo(path).fileName()));
        }
    }
    if (!step.trashed.isEmpty()) {
        pushUndo(step);
    }
}

void FileOps::deletePermanently(const QStringList& paths)
{
    for (const QString& path : paths) {
        const QFileInfo info(path);
        const bool ok = info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!ok) {
            emit failed(QCoreApplication::translate("Files", "Couldn't delete \"%1\".").arg(info.fileName()));
        }
        // From the Recycle Bin: its info record goes too.
        if (path.startsWith(trashDir() + QStringLiteral("/files/"))) {
            QFile::remove(trashDir() + QStringLiteral("/info/") + info.fileName() + QStringLiteral(".trashinfo"));
        }
    }
}

void FileOps::emptyTrash()
{
    QPointer<FileOps> self(this);
    QThreadPool::globalInstance()->start([self] {
        const QStringList failed = vela::trash::emptyHome();
        QMetaObject::invokeMethod(qApp, [self, failed] {
            if (!self || failed.isEmpty()) return;
            emit self->failed(QCoreApplication::translate("Files",
                "Couldn't empty the Recycle Bin (%1 items could not be deleted).").arg(failed.size()));
        }, Qt::QueuedConnection);
    });
}

QString FileOps::originalLocation(const QString& trashedPath) const
{
    QFile info(trashDir() + QStringLiteral("/info/") + QFileInfo(trashedPath).fileName() + QStringLiteral(".trashinfo"));
    if (!info.open(QIODevice::ReadOnly)) {
        return {};
    }
    for (const QByteArray& line : info.readAll().split('\n')) {
        if (line.startsWith("Path=")) {
            return QUrl::fromPercentEncoding(line.mid(5).trimmed());
        }
    }
    return {};
}

void FileOps::restoreFromTrash(const QStringList& paths)
{
    for (const QString& path : paths) {
        const QString original = originalLocation(path);
        if (original.isEmpty()) {
            continue;
        }
        const QFileInfo target(original);
        QDir().mkpath(target.absolutePath());
        const QString destination = QDir(target.absolutePath()).filePath(uniqueNameIn(target.absolutePath(), target.fileName()));
        if (QFile::rename(path, destination)) {
            QFile::remove(trashDir() + QStringLiteral("/info/") + QFileInfo(path).fileName() + QStringLiteral(".trashinfo"));
        }
    }
}

// -------------------------------------------------------- compressing --

void FileOps::compress(const QStringList& paths, const QString& format)
{
    if (paths.isEmpty()) {
        return;
    }
    const QFileInfo first(paths.first());
    const QString directory = first.absolutePath();
    QStringList names;
    for (const QString& path : paths) {
        names.append(QFileInfo(path).fileName());
    }
    // The archive takes the name of the first item, like Windows.
    const QString base = first.isDir() ? first.fileName() : first.completeBaseName();
    const QString archive = uniqueNameIn(directory, base + u'.' + format);
    QStringList arguments { QStringLiteral("-a"), QStringLiteral("-cf"), archive, QStringLiteral("--") };
    arguments.append(names);
    if (runDetached(QStringLiteral("bsdtar"), arguments, directory)) {
        UndoStep step;
        step.label = QCoreApplication::translate("Files", "Compress");
        step.created.append(QDir(directory).filePath(archive));
        pushUndo(step);
        emit created(QDir(directory).filePath(archive), false);
    }
}

bool FileOps::isArchive(const QString& path) const
{
    static const QStringList suffixes { QStringLiteral(".zip"), QStringLiteral(".7z"), QStringLiteral(".tar"),
        QStringLiteral(".tar.gz"), QStringLiteral(".tgz"), QStringLiteral(".tar.xz"), QStringLiteral(".tar.zst"),
        QStringLiteral(".tar.bz2"), QStringLiteral(".rar") };
    return std::any_of(suffixes.cbegin(), suffixes.cend(),
        [&](const QString& s) { return path.endsWith(s, Qt::CaseInsensitive); });
}

void FileOps::extractAll(const QString& path)
{
    // "Extract all": into a folder named after the archive, next to it.
    const QFileInfo info(path);
    QString base = info.fileName();
    for (const QString& s : { QStringLiteral(".tar.gz"), QStringLiteral(".tar.xz"), QStringLiteral(".tar.zst"),
             QStringLiteral(".tar.bz2") }) {
        if (base.endsWith(s, Qt::CaseInsensitive)) {
            base.chop(s.size());
        }
    }
    if (base == info.fileName()) {
        base = info.completeBaseName();
    }
    const QString folder = uniqueNameIn(info.absolutePath(), base);
    const QString target = QDir(info.absolutePath()).filePath(folder);
    QDir().mkpath(target);
    runDetached(QStringLiteral("bsdtar"), { QStringLiteral("-xf"), path, QStringLiteral("-C"), target }, info.absolutePath());
    UndoStep step;
    step.label = QCoreApplication::translate("Files", "Extract");
    step.created.append(target);
    pushUndo(step);
    emit created(target, false);
}

// ---------------------------------------------------------------- undo --

void FileOps::pushUndo(UndoStep step)
{
    m_undo.append(std::move(step));
    if (m_undo.size() > 30) {
        m_undo.removeFirst();
    }
    emit undoChanged();
}

QString FileOps::undoText() const
{
    return m_undo.isEmpty() ? QString() : m_undo.last().label;
}

void FileOps::undo()
{
    if (m_undo.isEmpty()) {
        return;
    }
    const UndoStep step = m_undo.takeLast();
    emit undoChanged();
    for (const auto& [current, original] : step.moves) {
        if (!QFileInfo::exists(original)) {
            QDir().mkpath(QFileInfo(original).absolutePath());
            if (!QFile::rename(current, original)) {
                // Between different disks: brought back by copying.
                startTransfer({ { current }, QFileInfo(original).absolutePath(), true }, QStringLiteral("keep"));
            }
        }
    }
    for (const QString& path : step.created) {
        QFile::moveToTrash(path);
    }
    for (qsizetype i = 0; i < step.trashed.size(); ++i) {
        // From the Recycle Bin (…/Trash/files/name) back to its place, without
        // the record in …/Trash/info.
        const QString inTrash = step.trashed.at(i);
        if (!QFileInfo::exists(step.originals.at(i)) && QFile::rename(inTrash, step.originals.at(i))) {
            const QFileInfo info(inTrash);
            QFile::remove(info.absoluteDir().absoluteFilePath(
                QStringLiteral("../info/") + info.fileName() + QStringLiteral(".trashinfo")));
        }
    }
}

static void sendShellFileDialog(const QByteArray& action, const QStringList& paths)
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    QLocalSocket socket;
    socket.connectToServer(runtimeDir + QStringLiteral("/vela-shell-") + display + QStringLiteral(".sock"));
    if (!socket.waitForConnected(300)) return;

    // A running shell may predate the screen-aware JSON format. Probe it
    // before sending the action, keeping legacy array requests compatible.
    socket.write("dialog-protocol\n");
    if (!socket.waitForBytesWritten(300)) return;
    if (!socket.canReadLine()) socket.waitForReadyRead(100);
    const bool screenAware = socket.canReadLine() && socket.readLine().trimmed() == "2";

    QByteArray payload;
    if (screenAware) {
        QJsonObject request;
        request.insert(QStringLiteral("paths"), QJsonArray::fromStringList(paths));
        if (QWindow* window = QGuiApplication::focusWindow()) {
            if (QScreen* screen = window->screen()) {
                request.insert(QStringLiteral("screen"), screen->name());
            }
        }
        payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    } else {
        payload = QJsonDocument(QJsonArray::fromStringList(paths)).toJson(QJsonDocument::Compact);
    }
    socket.write(action + ' ' + payload + '\n');
    socket.waitForBytesWritten(300);
}

void FileOps::showProperties(const QStringList& paths)
{
    sendShellFileDialog("properties", paths);
}

void FileOps::share(const QStringList& paths)
{
    sendShellFileDialog("share", paths);
}

void FileOps::chooseApp(const QString& path)
{
    sendShellFileDialog("open-with", { path });
}

void FileOps::newShortcut(const QString& directory)
{
    sendShellFileDialog("new-shortcut", { directory });
}

// ---------------------------------------------------------- shortcuts --

void FileOps::createLinks(const QStringList& paths)
{
    UndoStep step;
    step.label = QCoreApplication::translate("Files", "New");
    QString last;
    for (const QString& path : paths) {
        QString directory = QFileInfo(path).absolutePath();
        if (!QFileInfo(directory).isWritable()) {
            // Like Windows: not possible here, it goes on the desktop.
            directory = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        }
        const QString link = createLink(path, directory);
        if (link.isEmpty()) {
            emit failed(QCoreApplication::translate("Files", "Couldn't create a shortcut to \"%1\".").arg(QFileInfo(path).fileName()));
            continue;
        }
        step.created.append(link);
        last = link;
    }
    if (!step.created.isEmpty()) {
        pushUndo(step);
        emit created(last, false);
    }
}

void FileOps::pasteLinks(const QString& directory)
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) {
        return;
    }
    UndoStep step;
    step.label = QCoreApplication::translate("Files", "New");
    QString last;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile() || !QFileInfo::exists(url.toLocalFile())) {
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        // In the original's own folder the name says it's a shortcut.
        const QString link = createLink(info.absoluteFilePath(), directory,
            info.absolutePath() == QDir(directory).absolutePath() ? QString() : info.fileName());
        if (!link.isEmpty()) {
            step.created.append(link);
            last = link;
        }
    }
    if (step.created.isEmpty()) {
        emit failed(QCoreApplication::translate("Files", "Couldn't create shortcuts here."));
        return;
    }
    pushUndo(step);
    emit created(last, false);
}

bool FileOps::isLink(const QString& path) const
{
    return QFileInfo(path).isSymLink();
}

QString FileOps::linkTarget(const QString& path) const
{
    return QFileInfo(path).symLinkTarget();
}

// -------------------------------------------- preview and details --

QVariantMap FileOps::details(const QString& path) const
{
    static const QMimeDatabase mimes;
    const QFileInfo info(path);
    QVariantMap out {
        { QStringLiteral("name"), info.fileName().isEmpty() ? path : info.fileName() },
        { QStringLiteral("location"), info.absolutePath() },
        { QStringLiteral("isDir"), info.isDir() },
        { QStringLiteral("modified"), info.lastModified() },
        { QStringLiteral("created"), info.birthTime().isValid() ? info.birthTime() : info.metadataChangeTime() },
    };
    const QMimeType mime = mimes.mimeTypeForFile(info);
    out[QStringLiteral("mime")] = mime.name();
    out[QStringLiteral("icon")] = info.isDir() ? QStringLiteral("folder") : mime.iconName();
    out[QStringLiteral("type")] = info.isDir() ? QCoreApplication::translate("Files", "File folder") : mime.comment();
    if (info.isDir()) {
        out[QStringLiteral("items")] = int(QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size());
    } else {
        out[QStringLiteral("size")] = double(info.size());
        out[QStringLiteral("sizeText")] = ::formatSize(info.size());
        // Only regular files are read: opening a FIFO with no writer blocks.
        if (info.isFile()) {
            QImageReader reader(path);
            if (reader.canRead()) {
                const QSize size = reader.size();
                if (size.isValid()) {
                    out[QStringLiteral("width")] = size.width();
                    out[QStringLiteral("height")] = size.height();
                }
            }
        }
    }
    if (info.isSymLink()) {
        out[QStringLiteral("target")] = info.symLinkTarget();
    }
    return out;
}

QString FileOps::previewText(const QString& path) const
{
    static const QMimeDatabase mimes;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > 8 * 1024 * 1024) {
        return {};
    }
    const QMimeType mime = mimes.mimeTypeForFile(info);
    if (!mime.inherits(QStringLiteral("text/plain")) && mime.name() != QLatin1String("application/json")
        && mime.name() != QLatin1String("application/xml")) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    // The beginning is enough: the pane shows a few dozen lines.
    QString text = QString::fromUtf8(file.read(32 * 1024));
    text.replace(u'\t', QStringLiteral("    "));
    return text.isEmpty() ? QStringLiteral(" ") : text;
}

bool FileOps::isImage(const QString& path) const
{
    return QImageReader(path).canRead();
}

bool FileOps::pdfSupported() const
{
#ifdef VELA_FILES_PDF
    return true;
#else
    return false;
#endif
}

bool FileOps::mediaSupported() const
{
#ifdef VELA_FILES_MEDIA
    return true;
#else
    return false;
#endif
}

void FileOps::peek(const QString& path)
{
    QPointer<FileOps> self(this);
    QThreadPool::globalInstance()->start([self, path] {
        if (!self) {
            return;
        }
        QVariantMap out = self->details(path);
        const bool isDir = out.value(QStringLiteral("isDir")).toBool();
        // A FIFO, socket or device is never opened: it could block this thread for good.
        const bool regular = QFileInfo(path).isFile();
        QString kind = peek::classify(out.value(QStringLiteral("mime")).toString(), isDir,
            regular && QImageReader(path).canRead(), self->pdfSupported(), self->mediaSupported());
        if (!isDir && !regular) {
            kind = QStringLiteral("other");
        } else if (kind == QLatin1String("other")) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly) && peek::looksLikeText(file.read(peek::sniffBytes))) {
                kind = QStringLiteral("text");
            }
        }
        out[QStringLiteral("kind")] = kind;
        if (kind == QLatin1String("folder")) {
            // A bounded walk: a huge tree must not hold the preview.
            qint64 total = 0;
            int seen = 0;
            QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (it.hasNext() && seen < 20000) {
                it.next();
                total += it.fileInfo().size();
                ++seen;
            }
            out[QStringLiteral("sizeText")] = (it.hasNext() ? QStringLiteral("> ") : QString()) + ::formatSize(total);
        } else if (kind == QLatin1String("text")) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly)) {
                bool cut = false;
                const QString text = peek::truncateText(file.read(peek::maxTextBytes), file.size(), peek::maxTextBytes, &cut);
                out[QStringLiteral("text")] = text.isEmpty() ? QStringLiteral(" ") : text;
                out[QStringLiteral("truncated")] = cut;
            }
        }
        if (self) {
            QMetaObject::invokeMethod(self, [self, path, out] { if (self) emit self->peekReady(path, out); }, Qt::QueuedConnection);
        }
    });
}

// ------------------------------------------------------------- misc --

QString FileOps::formatSize(double bytes) const
{
    return ::formatSize(qint64(bytes));
}

QStringList FileOps::subfolders(const QString& path, bool hidden) const
{
    QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot;
    if (hidden) {
        filters |= QDir::Hidden;
    }
    QStringList names = QDir(path).entryList(filters);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), collator);
    return names;
}

QString FileOps::resolve(const QString& text, const QString& base) const
{
    QString path = text.trimmed();
    if (path.startsWith(QLatin1String("file://"))) {
        path = QUrl(path).toLocalFile();
    }
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/"))) {
        path = QDir::homePath() + path.mid(1);
    }
    if (!path.startsWith(u'/')) {
        path = QDir(base.startsWith(u'/') ? base : QDir::homePath()).filePath(path);
    }
    const QFileInfo info(path);
    return info.isDir() ? info.absoluteFilePath() : QString();
}

void FileOps::newWindow(const QString& location) const
{
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
        location.startsWith(u'/') ? QStringList { location } : QStringList {});
}
