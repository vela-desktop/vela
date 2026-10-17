// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractItemModel>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <functional>
#include <memory>

class AppModel;
class TransferJobsModel;

// What File Explorer does with files, like Windows: open, clipboard (cut,
// copy, paste, also to and from Dolphin and Nautilus), New, rename, Recycle
// Bin and deletion, compress and extract, and Undo (Ctrl+Z). Long copies and
// moves run in another thread with their progress (Windows' "N items copying"
// panel); if the destination already has files with the same name, it asks
// first what to do.
class FileOps : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    // [{id, title, done, total, current, finished, error}]
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(QAbstractItemModel* jobModel READ jobModel CONSTANT)
    Q_PROPERTY(int activeJobCount READ activeJobCount NOTIFY jobsChanged)

public:
    explicit FileOps(AppModel* apps, QObject* parent = nullptr);
    ~FileOps() override;

    bool canPaste() const;
    QSet<QString> cutPaths() const;
    QString undoText() const;
    QVariantList jobs() const;
    QAbstractItemModel* jobModel() const;
    int activeJobCount() const;
    bool activeOnDevice(const QString& device) const;
    // Includes mounted filesystems nested below a copied directory.
    static bool pathsOnDevice(const QStringList& sources, const QString& destination, const QString& device);
    // Refuse transfers started while UDisks is unmounting their filesystem.
    void setEjectGuard(std::function<bool(const QStringList&, const QString&)> guard);

    // Opening: folders are opened by Explorer itself; files with the default
    // app.
    Q_INVOKABLE void open(const QStringList& paths);
    Q_INVOKABLE void openWith(const QString& appId, const QString& path);
    Q_INVOKABLE QVariantList appsFor(const QString& path) const;

    Q_INVOKABLE void copy(const QStringList& paths);
    Q_INVOKABLE void cut(const QStringList& paths);
    Q_INVOKABLE void paste(const QString& directory);
    Q_INVOKABLE void copyAsPath(const QStringList& paths);
    // Files dropped here (dragged): `action` 1 copy, 2 move, 0 like Windows
    // (moved if on the same disk, otherwise copied).
    Q_INVOKABLE void drop(const QStringList& urls, const QString& directory, int action);
    // Only shell-initiated transfers report their committed Undo step to DesktopModel.
    void dropFromDesktop(const QStringList& urls, const QString& directory, int action);

    // New: they return the path, to rename it right away.
    Q_INVOKABLE QString createFolder(const QString& directory);
    Q_INVOKABLE QString createFile(const QString& directory, const QString& templatePath);
    Q_INVOKABLE QVariantList templates() const; // [{name, icon, path}]
    // The new name, or an error message starting with "!".
    Q_INVOKABLE QString rename(const QString& path, const QString& newName);

    Q_INVOKABLE void trash(const QStringList& paths);
    Q_INVOKABLE void deletePermanently(const QStringList& paths);
    Q_INVOKABLE void emptyTrash();
    Q_INVOKABLE void restoreFromTrash(const QStringList& paths); // files in …/Trash/files
    Q_INVOKABLE QString originalLocation(const QString& trashedPath) const;

    Q_INVOKABLE void compress(const QStringList& paths, const QString& format);
    Q_INVOKABLE bool isArchive(const QString& path) const;
    Q_INVOKABLE void extractAll(const QString& path);

    Q_INVOKABLE void undo();
    Q_INVOKABLE void cancelJob(int id);
    Q_INVOKABLE void dismissJob(int id);

    // Conflicts: "replace", "skip", "keep" (keep both) or "cancel".
    Q_INVOKABLE void resolveConflict(const QString& choice);
    Q_INVOKABLE QString formatSize(double bytes) const;
    // The subfolders (for the arrow between path pieces), sorted.
    Q_INVOKABLE QStringList subfolders(const QString& path, bool hidden) const;
    // "~/Documents" and the like: the real path, "" if it isn't a folder.
    Q_INVOKABLE QString resolve(const QString& text, const QString& base) const;
    Q_INVOKABLE void newWindow(const QString& location) const;
    // The Properties window is the shell's (the same as the desktop's).
    Q_INVOKABLE void showProperties(const QStringList& paths);
    // Also Share, "Choose another app" and New > Shortcut.
    Q_INVOKABLE void share(const QStringList& paths);
    Q_INVOKABLE void chooseApp(const QString& path);
    Q_INVOKABLE void newShortcut(const QString& directory);

    // Shortcuts (links.h): "Create shortcut" puts them next to the originals
    // (or on the desktop, when that isn't writable); "Paste shortcut" creates
    // here those of the files on the clipboard.
    Q_INVOKABLE void createLinks(const QStringList& paths);
    Q_INVOKABLE void pasteLinks(const QString& directory);
    Q_INVOKABLE bool isLink(const QString& path) const;
    Q_INVOKABLE QString linkTarget(const QString& path) const;

    // The details pane: {name, type, size, sizeText, modified, created, isDir,
    // items, mime, width, height, location}.
    Q_INVOKABLE QVariantMap details(const QString& path) const;
    // The preview pane: the beginning of a text file (empty if it isn't one),
    // and whether it's an image Qt can read.
    Q_INVOKABLE QString previewText(const QString& path) const;
    Q_INVOKABLE bool isImage(const QString& path) const;

signals:
    void clipboardChanged();
    void undoChanged();
    void jobsChanged();
    // The destination already has something with the same name: the view asks.
    void conflict(int count, const QString& firstName, const QString& directory);
    // A file just created or arrived: the view selects it (and has it
    // renamed).
    void created(const QString& path, bool rename);
    void failed(const QString& message);

private:
    struct UndoStep {
        QString label;
        QList<QPair<QString, QString>> moves; // (where it is now, where it was)
        QStringList created;
        QStringList trashed; // where they are in the Recycle Bin
        QStringList originals; // and where they came from
    };
    struct Job;
    struct Transfer {
        QStringList sources;
        QString directory;
        bool move = false;
        bool fromDesktop = false;
    };

    void pushUndo(UndoStep step);
    void startTransfer(const Transfer& transfer, const QString& policy);
    void requestTransfer(const Transfer& transfer);
    void dropInternal(const QStringList& urls, const QString& directory, int action, bool fromDesktop);
    void updateJob(int id, qint64 done, qint64 total, const QString& current, bool finished, const QString& error);
    QVariantMap jobAsMap(const Job& job) const;

    AppModel* m_apps;
    QList<UndoStep> m_undo;
    QList<std::shared_ptr<Job>> m_jobs;
    TransferJobsModel* m_jobModel = nullptr;
    int m_nextJob = 1;
    Transfer m_pending; // waiting for the answer to the conflict
    std::function<bool(const QStringList&, const QString&)> m_ejectGuard;
};

// A free name in the folder: "name (2).ext", like Windows.
QString uniqueNameIn(const QString& directory, const QString& name);

// Progress work units; the same accounting is used by recursive copying.
qint64 transferWorkUnits(const QString& source);
