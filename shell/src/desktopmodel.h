// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

class AppModel;

// Desktop icons, like Windows 11: the files of the Desktop folder
// (XDG_DESKTOP_DIR, such as ~/Desktop) plus the Recycle Bin, in a grid filled
// by columns from the top left corner. Icons moved by hand stay where they are
// (saved), until sorted.
class DesktopModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString directory READ directory CONSTANT)
    // View: 0 large icons, 1 medium, 2 small.
    Q_PROPERTY(int iconSize READ iconSize WRITE setIconSize NOTIFY viewChanged)
    Q_PROPERTY(bool autoArrange READ autoArrange WRITE setAutoArrange NOTIFY viewChanged)
    Q_PROPERTY(bool alignToGrid READ alignToGrid WRITE setAlignToGrid NOTIFY viewChanged)
    Q_PROPERTY(bool showIcons READ showIcons WRITE setShowIcons NOTIFY viewChanged)
    // Sort by: 0 name, 1 size, 2 item type, 3 date modified.
    Q_PROPERTY(int sortMode READ sortMode NOTIFY viewChanged)
    // The grid's sizes (logical), for the chosen size.
    Q_PROPERTY(int iconPixels READ iconPixels NOTIFY viewChanged)
    Q_PROPERTY(int cellWidth READ cellWidth NOTIFY viewChanged)
    Q_PROPERTY(int cellHeight READ cellHeight NOTIFY viewChanged)
    // The area holding the icons (the output without the taskbar).
    Q_PROPERTY(qreal areaWidth READ areaWidth WRITE setAreaWidth NOTIFY areaChanged)
    Q_PROPERTY(qreal areaHeight READ areaHeight WRITE setAreaHeight NOTIFY areaChanged)
    // "Undo …": the name of the last undoable action (empty: none).
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        UrlRole,
        IconRole,
        IsDirRole,
        IsAppRole, // a .desktop shortcut
        IsTrashRole,
        TypeRole,
        ModifiedRole,
        ThumbnailRole, // there is (or can be) a thumbnail
        XRole, // position in the grid (logical, inside the area)
        YRole,
    };

    explicit DesktopModel(AppModel* apps, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString directory() const { return m_dir; }
    int iconSize() const { return m_iconSize; }
    void setIconSize(int size);
    bool autoArrange() const { return m_autoArrange; }
    void setAutoArrange(bool on);
    bool alignToGrid() const { return m_alignToGrid; }
    void setAlignToGrid(bool on);
    bool showIcons() const { return m_showIcons; }
    void setShowIcons(bool on);
    int sortMode() const { return m_sortMode; }
    int iconPixels() const;
    int cellWidth() const;
    int cellHeight() const;
    qreal areaWidth() const { return m_areaWidth; }
    void setAreaWidth(qreal width);
    qreal areaHeight() const { return m_areaHeight; }
    void setAreaHeight(qreal height);
    QString undoText() const;
    bool canPaste() const;

    // Sorts (Sort by): icons line up again in that order.
    Q_INVOKABLE void sortBy(int mode);
    Q_INVOKABLE void refresh();
    // Dragged there (logical coordinates of the area): it snaps to the nearest
    // free cell with "Align icons to grid".
    Q_INVOKABLE void moveTo(const QString& path, qreal x, qreal y);

    Q_INVOKABLE void open(const QStringList& paths);
    Q_INVOKABLE bool rename(const QString& path, const QString& newName);
    Q_INVOKABLE void trash(const QStringList& paths);
    Q_INVOKABLE void emptyTrash();
    Q_INVOKABLE bool trashEmpty() const;
    Q_INVOKABLE void undo();
    // Receive committed transfer steps from the separate vela-files process.
    void recordExternalTransfer(const QByteArray& json);
    // New: they return the path, to rename it right away.
    Q_INVOKABLE QString createFolder();
    Q_INVOKABLE QString createFile(const QString& templatePath); // empty: text document
    // Document templates (XDG_TEMPLATES_DIR): [{name, icon, path}].
    Q_INVOKABLE QVariantList templates() const;
    Q_INVOKABLE void copy(const QStringList& paths);
    Q_INVOKABLE void cut(const QStringList& paths);
    Q_INVOKABLE void paste();
    Q_INVOKABLE void copyAsPath(const QStringList& paths);
    // Files dropped here by an app (dragged): on the desktop at point (x, y),
    // in a folder, or in the Recycle Bin ("trash:/"). Like Windows: moved if
    // on the same disk, otherwise copied.
    Q_INVOKABLE void drop(const QStringList& urls, const QString& target, qreal x, qreal y);
    // Compress to: "zip", "7z" or "tar".
    Q_INVOKABLE void compress(const QStringList& paths, const QString& format);

signals:
    void viewChanged();
    void areaChanged();
    void undoChanged();
    void clipboardChanged();
    // A file just created (New, Paste): the view selects it and, if asked, has
    // it renamed.
    void created(const QString& path, bool rename);
    void chooseAppRequested(const QString& path);

private:
    struct Item {
        QString name;
        QString path;
        QString icon;
        QString type; // type description, for sorting
        bool isDir = false;
        bool isApp = false;
        bool isTrash = false;
        bool hasThumbnail = false;
        qint64 size = 0;
        QDateTime modified;
        QPointF position; // chosen by hand (saved); (-1, -1): none
    };
    struct UndoStep {
        QString label; // "Rename", "Delete", "New", "Paste"
        QList<QPair<QString, QString>> moves; // from -> to, to go back
        QStringList created; // to remove (to the bin)
        QStringList trashed; // paths in the bin, to put back in place
        QStringList originals;
    };

    void reload();
    void layout(bool notify = true);
    void savePosition(const QString& name, const QPointF& position);
    QPointF cellPosition(int column, int row) const;
    int rows() const;
    QString uniqueName(const QString& name) const;
    static QString uniqueNameIn(const QString& dir, const QString& name);
    void pushUndo(UndoStep step);

    AppModel* m_apps;
    QString m_dir;
    QList<Item> m_items;
    QList<QPointF> m_layout; // where each icon shows
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    int m_iconSize = 1;
    int m_sortMode = 0;
    bool m_autoArrange = false;
    bool m_alignToGrid = true;
    bool m_showIcons = true;
    qreal m_areaWidth = 0;
    qreal m_areaHeight = 0;
    QList<UndoStep> m_undo;
    QString m_pendingSelect; // file just created, to announce when it appears
    bool m_pendingRename = false;
};
