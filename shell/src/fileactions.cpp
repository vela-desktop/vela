// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fileactions.h"
#include "mimeapps.h"

#include "links.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QStandardPaths>
#include <QVariantMap>

namespace {

bool has(const char* program)
{
    return !QStandardPaths::findExecutable(QLatin1String(program)).isEmpty();
}

} // namespace

FileActions::FileActions(QObject* parent)
    : QObject(parent)
{
}

bool FileActions::canKdeConnect() const
{
    return has("kdeconnect-cli");
}

bool FileActions::canEmail() const
{
    return has("xdg-email");
}

void FileActions::findDevices()
{
    if (!canKdeConnect() || m_searching) {
        return;
    }
    m_searching = true;
    emit devicesChanged();
    // kdeconnect-cli -a --id-name-only: "<id> <name>", one per line.
    auto* process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process] {
        QVariantList devices;
        for (const QString& line : QString::fromUtf8(process->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts)) {
            const qsizetype space = line.indexOf(u' ');
            if (space > 0) {
                devices.append(QVariantMap { { QStringLiteral("id"), line.left(space) },
                    { QStringLiteral("name"), line.mid(space + 1).trimmed() } });
            }
        }
        m_devices = devices;
        m_searching = false;
        emit devicesChanged();
        process->deleteLater();
    });
    process->start(QStringLiteral("kdeconnect-cli"), { QStringLiteral("-a"), QStringLiteral("--id-name-only") });
}

void FileActions::sendToDevice(const QString& id, const QStringList& paths)
{
    for (const QString& path : paths) {
        QProcess::startDetached(QStringLiteral("kdeconnect-cli"), { QStringLiteral("-d"), id, QStringLiteral("--share"), path });
    }
}

void FileActions::sendByEmail(const QStringList& paths)
{
    QStringList arguments;
    for (const QString& path : paths) {
        arguments << QStringLiteral("--attach") << path;
    }
    QProcess::startDetached(QStringLiteral("xdg-email"), arguments);
}

QString FileActions::typeName(const QString& path) const
{
    static const QMimeDatabase mimes;
    return mimes.mimeTypeForFile(path).comment();
}

QString FileActions::iconFor(const QString& path) const
{
    static const QMimeDatabase mimes;
    return QFileInfo(path).isDir() ? QStringLiteral("folder") : mimes.mimeTypeForFile(path).iconName();
}

QString FileActions::extension(const QString& path) const
{
    const QString suffix = QFileInfo(path).suffix();
    return suffix.isEmpty() ? QString() : u'.' + suffix.toLower();
}

void FileActions::setDefaultApp(const QString& appId, const QString& path) const
{
    static const QMimeDatabase mimes;
    const QString mime = mimes.mimeTypeForFile(path).name();
    if (!mime.isEmpty() && !appId.isEmpty()) {
        MimeApps::setDefault({ mime }, appId);
    }
}

QString FileActions::createShortcut(const QString& directory, const QString& target, const QString& name) const
{
    QString error;
    const QString path = ::createShortcut(directory, target, name.trimmed(), &error);
    return path.isEmpty() ? (error.isEmpty() ? QCoreApplication::translate("Desktop", "Couldn't create the shortcut.") : error) : QString();
}

QString FileActions::shortcutName(const QString& target, const QString& directory) const
{
    QString text = target.trimmed();
    if (text.startsWith(QLatin1String("~/"))) {
        text = QDir::homePath() + text.mid(1);
    }
    const QUrl url(text);
    if (!url.scheme().isEmpty() && url.scheme() != QLatin1String("file") && !text.startsWith(u'/')) {
        return url.host();
    }
    const QFileInfo info(url.isLocalFile() ? url.toLocalFile() : text);
    // Next to the original the name says it's a shortcut.
    if (info.absolutePath() == QDir(directory).absolutePath()) {
        const QString name = linkNameFor(info);
        return info.isDir() || info.suffix().isEmpty() ? name : name.chopped(info.suffix().size() + 1);
    }
    return info.isDir() ? info.fileName() : info.completeBaseName();
}

void FileActions::createLinks(const QStringList& paths, const QString& directory) const
{
    for (const QString& path : paths) {
        createLink(path, directory);
    }
}

bool FileActions::pasteLinks(const QString& directory) const
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) {
        return false;
    }
    bool any = false;
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile())) {
            const QFileInfo info(url.toLocalFile());
            any = !createLink(info.absoluteFilePath(), directory,
                      info.absolutePath() == QDir(directory).absolutePath() ? QString() : info.fileName())
                       .isEmpty()
                || any;
        }
    }
    return any;
}

bool FileActions::isFavorite(const QString& path) const
{
    return ::isFavorite(path);
}

void FileActions::setFavorite(const QString& path, bool on) const
{
    ::setFavorite(path, on);
}
