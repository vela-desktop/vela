// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 lowell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "themetokens.h" // generated from Theme.qml (common/CMakeLists.txt)

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtLogging>

#include <cmath>

// Theme files (theme=<name> in vela.conf, themes/<name>.conf): the colors and
// corner radii that Theme.qml takes over Vela's own (docs/configuration.md,
// "Theme files"). `key=value` lines and `#` comments:
//
//   <token>, dark.<token>, light.<token>   a Theme.qml token, for both modes
//                                          or one; a color (#RRGGBB,
//                                          #AARRGGBB) or a number, as the
//                                          token's type says
//   inherits=<name>                        the theme this one starts from
//   schema-version=1                       the format; another one is not used
//
// The first comment line names the theme ("# Rosé Pine: ...") and a
// "# Original author: <name>" line among the first comments credits it.
// Vela's own look is a theme too, themes/vela.conf ("Vela Official"): it sets
// no token, and it is the one used when theme= names none.
//
// The user's themes (~/.config/vela/themes/) win over the installed ones
// (the data directories: a package, or a nested run's XDG_DATA_DIRS).
namespace vela::themes {

inline const QString& subdirectory()
{
    static const QString dir = QStringLiteral("vela/themes");
    return dir;
}

// The theme when vela.conf names none: Vela Official.
inline QString defaultTheme()
{
    return QStringLiteral("vela");
}

// The format this Vela reads; a file asking for another one is not used.
constexpr int schemaVersion = 1;
// The longest inherits= chain: more is a mistake, not a theme.
constexpr int maxChain = 16;

// A name is a file name without ".conf": nothing that leads elsewhere.
inline bool validName(const QString& name)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$"));
    return safe.match(name).hasMatch();
}

// A token's type, from Theme.qml's property; nullptr when Theme.qml has none.
inline const Token* token(const QString& name)
{
    for (const Token& t : tokens) {
        if (name == QLatin1String(t.name)) {
            return &t;
        }
    }
    return nullptr;
}

// theme= of a vela.conf, the last one if there are several (like the
// compositor); Vela Official without one.
inline QString configuredName(const QString& velaConf)
{
    QString name;
    QFile file(velaConf);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.startsWith(QLatin1String("theme="))) {
                name = line.mid(6).trimmed();
            }
        }
    }
    return name.isEmpty() ? defaultTheme() : name;
}

// The files a name can be, the user's first; none when the name is not valid.
inline QStringList candidates(const QString& name)
{
    if (!validName(name)) {
        return {};
    }
    const QString relative = subdirectory() + u'/' + name + QStringLiteral(".conf");
    return QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, relative)
        + QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, relative);
}

// The directories themes are looked up in, the user's first: those that
// exist, for the list and the file watchers.
inline QStringList directories()
{
    QStringList roots { QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u'/' + subdirectory() };
    roots += QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, subdirectory(), QStandardPaths::LocateDirectory);
    roots += QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, subdirectory(), QStandardPaths::LocateDirectory);
    QStringList existing;
    for (const QString& root : std::as_const(roots)) {
        const QString path = QDir::cleanPath(root);
        if (!existing.contains(path) && QFileInfo(path).isDir()) {
            existing.append(path);
        }
    }
    return existing;
}

// A color as theme files write it: #RRGGBB or #AARRGGBB, nothing else.
inline bool validColor(const QString& value)
{
    static const QRegularExpression color(QStringLiteral("^#([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})$"));
    return color.match(value).hasMatch();
}

// The theme's file, or empty when the name is not valid or not installed.
inline QString locate(const QString& name)
{
    const QStringList files = candidates(name);
    return files.isEmpty() ? QString() : files.first();
}

// What is wrong in a theme, where. Settings lists them (Personalization >
// Themes) and the log has them in English (describe()).
struct Problem {
    enum Kind {
        NotKeyValue, // a line without key=value
        UnknownToken, // a key Theme.qml has no token for
        ExpectsColor, // a color token with something else
        ExpectsNumber, // a number token with something else
        InvalidParent, // inherits= with a name that can't be a theme
        MissingParent, // inherits= a theme that isn't installed
        InheritanceCycle, // inherits= back into the chain
        ChainTooLong, // more than maxChain themes
        UnsupportedSchema, // schema-version= other than schemaVersion
    };
    Kind kind;
    QString file;
    int line = 0;
    QString key;
    QString value;
};

inline QString kindName(Problem::Kind kind)
{
    switch (kind) {
    case Problem::NotKeyValue: return QStringLiteral("not-key-value");
    case Problem::UnknownToken: return QStringLiteral("unknown-token");
    case Problem::ExpectsColor: return QStringLiteral("expects-color");
    case Problem::ExpectsNumber: return QStringLiteral("expects-number");
    case Problem::InvalidParent: return QStringLiteral("invalid-parent");
    case Problem::MissingParent: return QStringLiteral("missing-parent");
    case Problem::InheritanceCycle: return QStringLiteral("inheritance-cycle");
    case Problem::ChainTooLong: return QStringLiteral("chain-too-long");
    case Problem::UnsupportedSchema: return QStringLiteral("unsupported-schema");
    }
    return {};
}

// For the log: "theme <file>:<line>: <what>, and what happens".
inline QString describe(const Problem& p)
{
    QString what;
    switch (p.kind) {
    case Problem::NotKeyValue:
        what = QStringLiteral("not a key=value line, skipped");
        break;
    case Problem::UnknownToken:
        what = QStringLiteral("%1 is not a theme token, skipped").arg(p.key);
        break;
    case Problem::ExpectsColor:
        what = QStringLiteral("%1 is a color, #RRGGBB or #AARRGGBB, not \"%2\": skipped").arg(p.key, p.value);
        break;
    case Problem::ExpectsNumber:
        what = QStringLiteral("%1 is a number, not \"%2\": skipped").arg(p.key, p.value);
        break;
    case Problem::InvalidParent:
        what = QStringLiteral("inherits=%1 is not a theme name: inherits nothing").arg(p.value);
        break;
    case Problem::MissingParent:
        what = QStringLiteral("inherits=%1, which is not installed: inherits nothing").arg(p.value);
        break;
    case Problem::InheritanceCycle:
        what = QStringLiteral("inherits=%1, which inherits back from this theme: the chain stops here").arg(p.value);
        break;
    case Problem::ChainTooLong:
        what = QStringLiteral("inherits=%1 makes a chain of more than %2 themes: it stops here").arg(p.value).arg(maxChain);
        break;
    case Problem::UnsupportedSchema:
        what = QStringLiteral("schema-version=%1, and this Vela reads %2: the file is not used").arg(p.value).arg(schemaVersion);
        break;
    }
    return p.line > 0 ? QStringLiteral("theme %1:%2: %3").arg(p.file).arg(p.line).arg(what)
                      : QStringLiteral("theme %1: %2").arg(p.file, what);
}

// For Settings: { kind, file, line, key, value }.
inline QVariantMap toVariant(const Problem& p)
{
    return { { QStringLiteral("kind"), kindName(p.kind) }, { QStringLiteral("file"), p.file },
        { QStringLiteral("line"), p.line }, { QStringLiteral("key"), p.key }, { QStringLiteral("value"), p.value } };
}

// One file: its tokens (a color as QColor, a number as double), what it
// inherits and its format, and what is wrong in it (each also logged).
struct File {
    QVariantMap tokens;
    QString parent;
    int parentLine = 0;
    QString schema; // empty: not given, the current one
    int schemaLine = 0;
    QList<Problem> problems;
};

inline File read(QIODevice& in, const QString& origin = QString())
{
    File file;
    const auto problem = [&file, &origin](Problem::Kind kind, int line, const QString& key = {}, const QString& value = {}) {
        file.problems.append({ kind, origin, line, key, value });
        qWarning("%s", qPrintable(describe(file.problems.last())));
    };
    int number = 0;
    while (!in.atEnd()) {
        ++number;
        const QString line = QString::fromUtf8(in.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) {
            continue;
        }
        const qsizetype equals = line.indexOf(u'=');
        const QString key = equals > 0 ? line.left(equals).trimmed() : QString();
        const QString value = equals > 0 ? line.mid(equals + 1).trimmed() : QString();
        if (key.isEmpty() || value.isEmpty()) {
            problem(Problem::NotKeyValue, number);
            continue;
        }
        if (key == QLatin1String("inherits")) {
            file.parent = value;
            file.parentLine = number;
            continue;
        }
        if (key == QLatin1String("schema-version")) {
            file.schema = value;
            file.schemaLine = number;
            continue;
        }
        const QString name = key.startsWith(QLatin1String("dark.")) || key.startsWith(QLatin1String("light."))
            ? key.section(u'.', 1)
            : key;
        const Token* t = token(name);
        if (!t) {
            problem(Problem::UnknownToken, number, key, value);
        } else if (t->kind == TokenKind::Color) {
            if (validColor(value)) {
                file.tokens.insert(key, QColor::fromString(value));
            } else {
                problem(Problem::ExpectsColor, number, key, value);
            }
        } else {
            bool isNumber = false;
            const double d = value.toDouble(&isNumber);
            if (isNumber && std::isfinite(d)) {
                file.tokens.insert(key, d);
            } else {
                problem(Problem::ExpectsNumber, number, key, value);
            }
        }
    }
    return file;
}

// A theme as Theme.qml takes it: its tokens over its parents', the files it
// was made of (the theme's first) and everything wrong on the way. A theme
// that isn't installed has no files: Vela's own look.
struct Resolved {
    QVariantMap tokens;
    QStringList files;
    QList<Problem> problems;
};

inline Resolved resolve(const QString& name)
{
    Resolved resolved;
    QList<QVariantMap> chain; // the theme first, then its parents
    QString path = locate(name);
    while (!path.isEmpty()) {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            break;
        }
        const QString current = path;
        File file = read(in, current);
        resolved.problems += file.problems;
        const auto problem = [&resolved, &current](Problem::Kind kind, int line, const QString& value) {
            resolved.problems.append({ kind, current, line, {}, value });
            qWarning("%s", qPrintable(describe(resolved.problems.last())));
        };
        if (!file.schema.isEmpty() && file.schema != QString::number(schemaVersion)) {
            problem(Problem::UnsupportedSchema, file.schemaLine, file.schema);
            break;
        }
        resolved.files.append(current);
        chain.append(file.tokens);
        path.clear();
        if (file.parent.isEmpty()) {
            break;
        }
        // The first file of that name not already in the chain: a theme of
        // the user's can start from the installed one of the same name.
        const QStringList parents = candidates(file.parent);
        for (const QString& candidate : parents) {
            if (!resolved.files.contains(candidate)) {
                path = candidate;
                break;
            }
        }
        if (!validName(file.parent)) {
            problem(Problem::InvalidParent, file.parentLine, file.parent);
        } else if (parents.isEmpty()) {
            problem(Problem::MissingParent, file.parentLine, file.parent);
        } else if (path.isEmpty()) {
            problem(Problem::InheritanceCycle, file.parentLine, file.parent);
        } else if (resolved.files.size() >= maxChain) {
            problem(Problem::ChainTooLong, file.parentLine, file.parent);
            path.clear();
        }
    }
    for (auto it = chain.crbegin(); it != chain.crend(); ++it) {
        resolved.tokens.insert(*it);
    }
    return resolved;
}

// A token's value as Theme.qml's themed() finds it: "dark.<key>" or
// "light.<key>" for the mode, else "<key>"; invalid when the theme lacks it.
inline QVariant themed(const QVariantMap& theme, const QString& key, bool light)
{
    const QString modeKey = (light ? QStringLiteral("light.") : QStringLiteral("dark.")) + key;
    return theme.contains(modeKey) ? theme.value(modeKey) : theme.value(key);
}

// The installed themes, by name: { name, text, author }. The text is the
// name the first line gives ("# Rosé Pine: ..."), else the file name; the
// author, the "# Original author:" line's ("" without one). The user's win
// over the installed ones of the same name; Vela Official comes first.
inline QVariantList available()
{
    QMap<QString, QString> files; // sorted by name; the first root wins, as in candidates()
    for (const QString& root : directories()) {
        for (const QString& file : QDir(root).entryList({ QStringLiteral("*.conf") }, QDir::Files)) {
            if (!files.contains(file.chopped(5)) && validName(file.chopped(5))) {
                files.insert(file.chopped(5), root + u'/' + file);
            }
        }
    }
    QVariantList list;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QString text = it.key();
        QString author;
        QFile file(it.value());
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // The leading comments: the name on the first line, the author below.
            for (bool first = true; !file.atEnd(); first = false) {
                const QString line = QString::fromUtf8(file.readLine()).trimmed();
                if (!line.startsWith(u'#')) {
                    break;
                }
                const QString comment = line.mid(1).trimmed();
                const qsizetype colon = comment.indexOf(u':');
                if (first && colon > 0) {
                    text = comment.left(colon).trimmed();
                } else if (comment.startsWith(QLatin1String("Original author:"))) {
                    author = comment.mid(16).trimmed();
                }
            }
        }
        const QVariantMap entry { { QStringLiteral("name"), it.key() }, { QStringLiteral("text"), text },
            { QStringLiteral("author"), author } };
        if (it.key() == defaultTheme()) {
            list.prepend(entry);
        } else {
            list.append(entry);
        }
    }
    return list;
}

} // namespace vela::themes
