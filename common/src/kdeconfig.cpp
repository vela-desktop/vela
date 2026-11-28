// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "kdeconfig.h"

#include <QByteArrayList>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QVariant>

#include <algorithm>

namespace vela::kde {

namespace {

// The flags of a suffix made only of "[$x]" parts ("" when there is none);
// nullopt when another bracket is in it: a locale ("key[it]") or a subgroup
// ("[Group][Sub]").
std::optional<QString> flagsOf(QStringView rest)
{
    QString flags;
    while (!rest.isEmpty()) {
        const qsizetype close = rest.indexOf(u']');
        if (!rest.startsWith(u"[$") || close < 0) {
            return std::nullopt;
        }
        flags += rest.mid(2, close - 2);
        rest = rest.mid(close + 1);
    }
    return flags;
}

// One line of the file, trimmed as KConfig trims it.
struct Line {
    enum Kind { Blank, Comment, Group, Key } kind = Blank;
    QString name; // the group, or the key without its flags
    std::optional<QString> flags; // nullopt: a locale or a subgroup
    QString value; // as written (escaped)
};

Line parse(const QString& raw)
{
    Line line;
    const QString text = raw.trimmed();
    if (text.isEmpty()) {
        return line;
    }
    if (text.startsWith(u'#')) {
        line.kind = Line::Comment;
        return line;
    }
    if (text.startsWith(u'[')) {
        line.kind = Line::Group;
        const qsizetype close = text.indexOf(u']');
        if (close < 0) {
            return line; // no name: matches no group
        }
        line.name = text.mid(1, close - 1);
        line.flags = flagsOf(QStringView(text).mid(close + 1));
        return line;
    }
    const qsizetype eq = text.indexOf(u'=');
    if (eq <= 0) {
        line.kind = Line::Comment; // not an entry: kept, never matched
        return line;
    }
    line.kind = Line::Key;
    const QString key = text.left(eq).trimmed();
    const qsizetype bracket = key.indexOf(u'[');
    line.name = bracket < 0 ? key : key.left(bracket);
    line.flags = bracket < 0 ? QString() : flagsOf(QStringView(key).mid(bracket));
    line.value = text.mid(eq + 1).trimmed();
    return line;
}

bool readAll(const QString& path, QByteArray* data)
{
    QFile file(path);
    if (!file.exists()) {
        data->clear();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    *data = file.readAll();
    return file.error() == QFileDevice::NoError;
}

// Applies one entry to the lines; false when KDE marks it immutable.
bool apply(QStringList& lines, const Entry& entry)
{
    bool inside = false;
    bool groupFound = false;
    qsizetype groupEnd = -1; // the last line that belongs to the group
    QList<qsizetype> keyLines;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const Line line = parse(lines[i]);
        switch (line.kind) {
        case Line::Group:
            // "[$i]" alone (at the top of the file) makes the whole file
            // immutable.
            if (line.name == QLatin1String("$i")) {
                return false;
            }
            inside = line.flags && line.name == entry.group;
            if (inside) {
                if (line.flags->contains(u'i')) {
                    return false;
                }
                groupFound = true;
                groupEnd = i;
            }
            break;
        case Line::Key:
            if (inside) {
                groupEnd = i;
                if (line.flags && line.name == entry.key) {
                    if (line.flags->contains(u'i')) {
                        return false;
                    }
                    keyLines.append(i);
                }
            }
            break;
        case Line::Comment:
            if (inside) {
                groupEnd = i;
            }
            break;
        case Line::Blank: break;
        }
    }
    const QString written = entry.key + u'=' + escape(entry.value);
    if (!keyLines.isEmpty()) {
        for (const qsizetype i : std::as_const(keyLines)) {
            lines[i] = written;
        }
    } else if (groupFound) {
        lines.insert(groupEnd + 1, written);
    } else {
        if (!lines.isEmpty() && !lines.constLast().trimmed().isEmpty()) {
            lines.append(QString());
        }
        lines.append(u'[' + entry.group + u']');
        lines.append(written);
    }
    return true;
}

void send(const QString& path, const QString& interface, const QString& member, const QVariantList& arguments = {})
{
    QDBusMessage message = QDBusMessage::createSignal(path, interface, member);
    message.setArguments(arguments);
    QDBusConnection::sessionBus().send(message);
}

} // namespace

QString configPath(const QString& file)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u'/' + file;
}

std::optional<QString> read(const QString& path, const QString& group, const QString& key)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    std::optional<QString> value;
    bool inside = false;
    while (!file.atEnd()) {
        const Line line = parse(QString::fromUtf8(file.readLine()));
        if (line.kind == Line::Group) {
            inside = line.flags && line.name == group;
        } else if (inside && line.kind == Line::Key && line.flags && line.name == key) {
            // "[$d]": KDE's mark for a key deleted over a system default.
            value = line.flags->contains(u'd') ? std::nullopt : std::optional(unescape(line.value));
        }
    }
    if (file.error() != QFileDevice::NoError) {
        return std::nullopt;
    }
    return value;
}

bool write(const QString& path, const QList<Entry>& entries)
{
    if (entries.isEmpty()) {
        return true;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    // KConfig's lock: a KDE app saving the same file at the same moment
    // can't interleave with this one. Never wait long on the GUI thread.
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(500)) {
        return false;
    }
    QByteArray original;
    if (!readAll(path, &original)) {
        return false;
    }
    QStringList lines;
    if (!original.isEmpty()) {
        lines = QString::fromUtf8(original).split(u'\n');
        if (lines.constLast().isEmpty()) {
            lines.removeLast(); // the final newline
        }
    }
    for (const Entry& entry : entries) {
        if (!apply(lines, entry)) {
            return false;
        }
    }
    const QByteArray data = (lines.join(u'\n') + u'\n').toUtf8();
    if (data == original) {
        return true;
    }
    // QSaveFile keeps the permissions of the file it replaces and writes
    // through a symbolic link to its target.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        return false;
    }
    return file.commit();
}

void notifyChanged(const QString& path, const QList<Entry>& entries)
{
    // KConfig's object path is "/" and the file's name: one D-Bus path
    // element, so only names made of letters, digits and '_' can have one.
    const QString name = QFileInfo(path).fileName();
    if (name.isEmpty() || !std::all_of(name.cbegin(), name.cend(), [](QChar ch) {
            return (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z') || (ch >= u'0' && ch <= u'9') || ch == u'_';
        })) {
        return;
    }
    QHash<QString, QByteArrayList> changes;
    for (const Entry& entry : entries) {
        QByteArrayList& keys = changes[entry.group];
        if (!keys.contains(entry.key.toUtf8())) {
            keys.append(entry.key.toUtf8());
        }
    }
    qDBusRegisterMetaType<QByteArrayList>();
    qDBusRegisterMetaType<QHash<QString, QByteArrayList>>();
    send(u'/' + name, QStringLiteral("org.kde.kconfig.notify"), QStringLiteral("ConfigChanged"),
        { QVariant::fromValue(changes) });
}

void notifyGlobalChange(GlobalChange change)
{
    send(QStringLiteral("/KGlobalSettings"), QStringLiteral("org.kde.KGlobalSettings"), QStringLiteral("notifyChange"),
        { static_cast<int>(change), 0 });
}

void refreshFonts()
{
    send(QStringLiteral("/KDEPlatformTheme"), QStringLiteral("org.kde.KDEPlatformTheme"),
        QStringLiteral("refreshFonts"));
}

void iconLoaderChanged()
{
    send(QStringLiteral("/KIconLoader"), QStringLiteral("org.kde.KIconLoader"), QStringLiteral("iconChanged"), { 0 });
}

QString escape(const QString& value)
{
    QString out;
    out.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar ch = value[i];
        if (ch == u' ' && (i == 0 || i == value.size() - 1)) {
            out += QStringLiteral("\\s"); // trimmed by readers otherwise
        } else if (ch == u'\\') {
            out += QStringLiteral("\\\\");
        } else if (ch == u'\n') {
            out += QStringLiteral("\\n");
        } else if (ch == u'\t') {
            out += QStringLiteral("\\t");
        } else if (ch == u'\r') {
            out += QStringLiteral("\\r");
        } else if (ch.unicode() < 32) {
            out += QStringLiteral("\\x%1").arg(int(ch.unicode()), 2, 16, QLatin1Char('0'));
        } else {
            out += ch;
        }
    }
    return out;
}

QString unescape(const QString& value)
{
    QString out;
    out.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar ch = value[i];
        if (ch != u'\\' || i + 1 == value.size()) {
            out += ch;
            continue;
        }
        const QChar next = value[++i];
        if (next == u's') {
            out += u' ';
        } else if (next == u't') {
            out += u'\t';
        } else if (next == u'n') {
            out += u'\n';
        } else if (next == u'r') {
            out += u'\r';
        } else if (next == u'\\' || next == u';' || next == u',') {
            out += next;
        } else if (next == u'x' && i + 2 < value.size()) {
            bool ok = false;
            const ushort code = value.mid(i + 1, 2).toUShort(&ok, 16);
            if (ok) {
                out += QChar(code);
                i += 2;
            } else {
                out += u'\\';
                out += next;
            }
        } else {
            out += u'\\'; // unknown: kept, as KConfig keeps it
            out += next;
        }
    }
    return out;
}

} // namespace vela::kde
