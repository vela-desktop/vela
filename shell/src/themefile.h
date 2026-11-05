// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 Lowell137
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "themetokens.h" // generated from Theme.qml and Styles.qml (common/CMakeLists.txt)

#include <QByteArray>
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
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtLogging>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

// Theme files (docs/configuration.md, "Theme files"): the values Theme.qml
// and Styles.qml take over Vela's own, and the blur the compositor draws
// behind the shell's panels. Two kinds, like Windhawk's theme and its three
// stylers:
//
//   theme=<name>                  themes/<name>.conf: a complete look, colors
//                                 and radii of the shell and the apps, and
//                                 any surface's keys
//   taskbar-style=<name>          themes/taskbar/<name>.conf
//   startmenu-style=<name>        themes/startmenu/<name>.conf
//   notifications-style=<name>    themes/notifications/<name>.conf (quick
//                                 settings and the notification center)
//
// A style is the user's own choice for one surface, over the theme: it may
// only set that surface's keys and the blur's (belongsTo()).
//
// `key=value` lines and `#` comments:
//
//   <token>, dark.<token>, light.<token>   a token, for both modes or one;
//                                          a color (#RRGGBB, #AARRGGBB), a
//                                          number or one of Vela's layouts
//                                          (variants()), as the token's type
//                                          says
//   blur-saturation, blur-strength,        the blur behind Vela's acrylic
//   blur-tint                              surfaces, which the compositor
//                                          draws (blurLook()); numbers, for
//                                          both modes
//   inherits=<name>                        the theme (or style of the same
//                                          surface) this one starts from
//   schema-version=1                       the format; another one is not used
//
// The first comment line names the file ("# Window Glass: ...") and a
// "# Original author: <name>" line among the first comments credits it.
// Vela's own look is a theme too, themes/vela.conf ("Vela Official"): it sets
// no token, and it is the one used when theme= names none.
//
// The user's files (~/.config/vela/themes/) win over the installed ones
// (the data directories: a package, or a nested run's XDG_DATA_DIRS).
namespace vela::themes {

// The layers, in the order they apply: the theme, then each surface's style.
enum Layer { Theme, Taskbar, StartMenu, Notifications, LayerCount };

struct LayerInfo {
    const char* key; // in vela.conf
    const char* directory; // under themes/, "" for the theme
};

inline const LayerInfo& layerInfo(Layer layer)
{
    static const std::array<LayerInfo, LayerCount> info { {
        { "theme", "" },
        { "taskbar-style", "taskbar" },
        { "startmenu-style", "startmenu" },
        { "notifications-style", "notifications" },
    } };
    return info[layer];
}

// "taskbar" -> Taskbar; "" -> Theme; anything else -> LayerCount.
inline Layer layerFor(const QString& surface)
{
    for (int i = 0; i < LayerCount; ++i) {
        if (surface == QLatin1String(layerInfo(Layer(i)).directory)) {
            return Layer(i);
        }
    }
    return LayerCount;
}

inline QString subdirectory(Layer layer = Theme)
{
    const QString dir = QString::fromLatin1(layerInfo(layer).directory);
    return dir.isEmpty() ? QStringLiteral("vela/themes") : QStringLiteral("vela/themes/") + dir;
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
// The largest file read: themes are a few hundred bytes, so anything bigger
// is a mistake, and never read into memory whole.
constexpr qint64 maxFileSize = 64 * 1024;

// A name is a file name without ".conf": nothing that leads elsewhere.
inline bool validName(const QString& name)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$"));
    return safe.match(name).hasMatch();
}

// The compositor's keys: the look of the blur behind Vela's acrylic surfaces
// (the taskbar, Start, quick settings...). Theme.qml has no token for them.
inline constexpr const char* blurKeys[] = { "blur-saturation", "blur-strength", "blur-tint" };

inline bool isBlurKey(const QString& key)
{
    return std::any_of(std::begin(blurKeys), std::end(blurKeys), [&key](const char* k) { return key == QLatin1String(k); });
}

// The compositor's "blur-look <scope> <saturation> <strength> <tint> <glass>"
// for a theme or a panel ("*", or its layer namespace): its blur-* keys, -1
// (Vela's own) for each it leaves out. glass: the panel has a style of its
// own (glass from 4% alpha). The compositor keeps each value within its range
// (compositor/src/scene/scene.h).
inline QByteArray blurLook(const QByteArray& scope, const QVariantMap& theme, bool glass = false)
{
    QByteArray line("blur-look " + scope);
    for (const char* key : blurKeys) {
        const QVariant value = theme.value(QLatin1String(key));
        line += ' ' + QByteArray::number(value.typeId() == QMetaType::Double ? value.toDouble() : -1.0);
    }
    return line + (glass ? " 1" : " 0");
}

// A token's type, from its property in Theme.qml or Styles.qml; nullptr when
// neither has it.
inline const Token* token(const QString& name)
{
    for (const Token& t : tokens) {
        if (name == QLatin1String(t.name)) {
            return &t;
        }
    }
    return nullptr;
}

// The layouts each variant key may name: Vela's own implementations, never
// code from a theme. Styles.qml lays each of them out.
inline const QMap<QString, QStringList>& variants()
{
    static const QMap<QString, QStringList> list {
        { QStringLiteral("taskbar-variant"),
            { QStringLiteral("default"), QStringLiteral("floating"), QStringLiteral("floating-split"), QStringLiteral("dock") } },
        { QStringLiteral("quicksettings-variant"), { QStringLiteral("default"), QStringLiteral("glass"), QStringLiteral("classic") } },
    };
    return list;
}

// Whether a key (without dark./light.) may be set by a file of this layer: a
// theme sets anything, a style its own surface's keys and the blur's.
inline bool belongsTo(const QString& key, Layer layer)
{
    switch (layer) {
    case Theme: return true;
    case Taskbar: return isBlurKey(key) || key == QLatin1String("taskbar") || key.startsWith(QLatin1String("taskbar-"));
    case StartMenu: return isBlurKey(key) || key.startsWith(QLatin1String("startmenu-"));
    case Notifications:
        return isBlurKey(key) || key.startsWith(QLatin1String("notifications-")) || key.startsWith(QLatin1String("quicksettings-"));
    case LayerCount: break;
    }
    return false;
}

// The chosen names in a vela.conf, by layer: "" where none, but the theme
// (defaultTheme()); the last line wins if a key is there twice (like the
// compositor).
using Choice = std::array<QString, LayerCount>;

inline Choice configured(const QString& velaConf)
{
    Choice choice;
    QFile file(velaConf);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            for (int i = 0; i < LayerCount; ++i) {
                const QString prefix = QLatin1String(layerInfo(Layer(i)).key) + u'=';
                if (line.startsWith(prefix)) {
                    choice[i] = line.mid(prefix.size()).trimmed();
                }
            }
        }
    }
    if (choice[Theme].isEmpty()) {
        choice[Theme] = defaultTheme();
    }
    return choice;
}

// theme= of a vela.conf; Vela Official without one.
inline QString configuredName(const QString& velaConf)
{
    return configured(velaConf)[Theme];
}

// The files a name can be, the user's first; none when the name is not valid.
inline QStringList candidates(const QString& name, Layer layer = Theme)
{
    if (!validName(name) || layer >= LayerCount) {
        return {};
    }
    const QString relative = subdirectory(layer) + u'/' + name + QStringLiteral(".conf");
    return QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, relative)
        + QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, relative);
}

// The directories a layer's files are looked up in, the user's first: those
// that exist, for the list and the file watchers.
inline QStringList directories(Layer layer = Theme)
{
    const QString sub = subdirectory(layer);
    QStringList roots { QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u'/' + sub };
    roots += QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, sub, QStandardPaths::LocateDirectory);
    roots += QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, sub, QStandardPaths::LocateDirectory);
    QStringList existing;
    for (const QString& root : std::as_const(roots)) {
        const QString path = QDir::cleanPath(root);
        if (!existing.contains(path) && QFileInfo(path).isDir()) {
            existing.append(path);
        }
    }
    return existing;
}

// What to watch for a layer's files: its directories (directories()) and,
// while the user's own doesn't exist yet, the nearest directory above it that
// does, so that one created later (~/.config/vela/themes/) is noticed and
// watched in turn.
inline QStringList watchedDirectories(Layer layer = Theme)
{
    QStringList dirs = directories(layer);
    QString user = QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u'/' + subdirectory(layer));
    while (!QFileInfo(user).isDir() && user.contains(u'/')) {
        user = user.section(u'/', 0, -2);
        if (user.isEmpty()) {
            user = QStringLiteral("/");
        }
        if (QFileInfo(user).isDir()) {
            if (!dirs.contains(user)) {
                dirs.append(user);
            }
            break;
        }
    }
    return dirs;
}

// A color as theme files write it: #RRGGBB or #AARRGGBB, nothing else.
inline bool validColor(const QString& value)
{
    static const QRegularExpression color(QStringLiteral("^#([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})$"));
    return color.match(value).hasMatch();
}

// The file of a theme or style, or empty when the name is not valid or not
// installed.
inline QString locate(const QString& name, Layer layer = Theme)
{
    const QStringList files = candidates(name, layer);
    return files.isEmpty() ? QString() : files.first();
}

// What is wrong in a theme or style, where. Settings lists them
// (Personalization > Themes) and the log has them in English (describe()).
struct Problem {
    enum Kind {
        NotKeyValue, // a line without key=value
        UnknownToken, // a key no token is for
        ExpectsColor, // a color token with something else
        ExpectsNumber, // a number token with something else
        UnknownVariant, // a variant key naming no layout of Vela's
        OtherSurface, // a style setting another surface's key
        InvalidParent, // inherits= with a name that can't be a theme
        MissingParent, // inherits= a theme that isn't installed
        InheritanceCycle, // inherits= back into the chain
        ChainTooLong, // more than maxChain themes
        UnsupportedSchema, // schema-version= other than schemaVersion
        TooLarge, // a file over maxFileSize
    };
    Kind kind;
    QString file;
    int line = 0;
    QString key;
    QString value;

    bool operator==(const Problem&) const = default;
};

inline QString kindName(Problem::Kind kind)
{
    switch (kind) {
    case Problem::NotKeyValue: return QStringLiteral("not-key-value");
    case Problem::UnknownToken: return QStringLiteral("unknown-token");
    case Problem::ExpectsColor: return QStringLiteral("expects-color");
    case Problem::ExpectsNumber: return QStringLiteral("expects-number");
    case Problem::UnknownVariant: return QStringLiteral("unknown-variant");
    case Problem::OtherSurface: return QStringLiteral("other-surface");
    case Problem::InvalidParent: return QStringLiteral("invalid-parent");
    case Problem::MissingParent: return QStringLiteral("missing-parent");
    case Problem::InheritanceCycle: return QStringLiteral("inheritance-cycle");
    case Problem::ChainTooLong: return QStringLiteral("chain-too-long");
    case Problem::UnsupportedSchema: return QStringLiteral("unsupported-schema");
    case Problem::TooLarge: return QStringLiteral("too-large");
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
    case Problem::UnknownVariant:
        what = QStringLiteral("%1=%2 is not one of Vela's layouts (%3): skipped")
                   .arg(p.key, p.value, variants().value(p.key.section(u'.', -1)).join(QStringLiteral(", ")));
        break;
    case Problem::OtherSurface:
        what = QStringLiteral("%1 is another surface's, not this style's: skipped").arg(p.key);
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
    case Problem::TooLarge:
        what = QStringLiteral("larger than %1 KiB: the file is not used").arg(maxFileSize / 1024);
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

// One file: its tokens (a color as QColor, a number as double, a variant as
// its name), what it inherits
// and its format, and what is wrong in it (each also logged).
struct File {
    QVariantMap tokens;
    QString parent;
    int parentLine = 0;
    QString schema; // empty: not given, the current one
    int schemaLine = 0;
    QList<Problem> problems;
};

inline File read(QIODevice& in, const QString& origin = QString(), Layer layer = Theme)
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
        const bool moded = key.startsWith(QLatin1String("dark.")) || key.startsWith(QLatin1String("light."));
        const QString name = moded ? key.section(u'.', 1) : key;
        const Token* t = token(name);
        const bool blur = !moded && isBlurKey(key);
        if (!t && !blur) {
            problem(Problem::UnknownToken, number, key, value);
            continue;
        }
        if (!belongsTo(name, layer)) {
            problem(Problem::OtherSurface, number, key, value);
            continue;
        }
        bool isNumber = false;
        const double d = value.toDouble(&isNumber);
        if (blur || t->kind == TokenKind::Number) {
            if (isNumber && std::isfinite(d)) {
                file.tokens.insert(key, d);
            } else {
                problem(Problem::ExpectsNumber, number, key, value);
            }
        } else if (t->kind == TokenKind::Color) {
            if (validColor(value)) {
                file.tokens.insert(key, QColor::fromString(value));
            } else {
                problem(Problem::ExpectsColor, number, key, value);
            }
        } else if (variants().value(name).contains(value)) {
            file.tokens.insert(key, value);
        } else {
            problem(Problem::UnknownVariant, number, key, value);
        }
    }
    return file;
}

// A theme or style as Theme.qml takes it: its tokens over its parents', the
// files it was made of (its own first) and everything wrong on the way. One
// that isn't installed has no files: Vela's own look.
struct Resolved {
    QVariantMap tokens;
    QStringList files;
    QList<Problem> problems;

    bool operator==(const Resolved&) const = default;
};

inline Resolved resolve(const QString& name, Layer layer = Theme)
{
    Resolved resolved;
    QList<QVariantMap> chain; // the file first, then its parents
    QString path = locate(name, layer);
    while (!path.isEmpty()) {
        const QString current = path;
        if (QFileInfo(path).size() > maxFileSize) {
            resolved.problems.append({ Problem::TooLarge, current, 0, {}, {} });
            qWarning("%s", qPrintable(describe(resolved.problems.last())));
            break;
        }
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            break;
        }
        File file = read(in, current, layer);
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
        const QStringList parents = candidates(file.parent, layer);
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

// The chosen theme and styles of a vela.conf, each resolved on its own (a
// panel's blur look comes from the theme and its own style only), and merged
// (what Theme.qml and Styles.qml take: a style's keys over the theme's).
struct Loaded {
    std::array<Resolved, LayerCount> layers;

    QVariantMap merged() const
    {
        QVariantMap map;
        for (const Resolved& layer : layers) {
            map.insert(layer.tokens);
        }
        return map;
    }
    // The theme with just one surface's style.
    QVariantMap withStyle(Layer layer) const
    {
        QVariantMap map = layers[Theme].tokens;
        if (layer != Theme && layer < LayerCount) {
            map.insert(layers[layer].tokens);
        }
        return map;
    }
    // Whether a surface has a look of its own: a style chosen for it and
    // installed, or the theme's keys for it (a complete look, Window Glass's
    // pill taskbar). Its blur is then rounded like it, glass from 4% alpha.
    // Colors Theme.qml shares (taskbar) and the blur's keys don't count.
    bool styled(Layer layer) const
    {
        if (layer == Theme || layer >= LayerCount) {
            return false;
        }
        if (!layers[layer].files.isEmpty()) {
            return true;
        }
        const QStringList keys = layers[Theme].tokens.keys();
        return std::any_of(keys.begin(), keys.end(), [layer](const QString& key) {
            const QString name = key.startsWith(QLatin1String("dark.")) || key.startsWith(QLatin1String("light."))
                ? key.section(u'.', 1)
                : key;
            return name != QLatin1String("taskbar") && !isBlurKey(name) && belongsTo(name, layer);
        });
    }
    // Every file read, for the watchers.
    QStringList files() const
    {
        QStringList all;
        for (const Resolved& layer : layers) {
            all += layer.files;
        }
        return all;
    }
    bool operator==(const Loaded&) const = default;
};

inline Loaded load(const Choice& choice)
{
    Loaded loaded;
    for (int i = 0; i < LayerCount; ++i) {
        if (!choice[i].isEmpty()) {
            loaded.layers[i] = resolve(choice[i], Layer(i));
        }
    }
    return loaded;
}

inline Loaded loadConfigured(const QString& velaConf)
{
    return load(configured(velaConf));
}

// The installed themes ("") or a surface's styles ("taskbar"...), by name:
// { name, text, author }. The text is the name the first line gives
// ("# Window Glass: ..."), else the file name; the author, the "# Original
// author:" line's ("" without one). The user's win over the installed ones
// of the same name; Vela Official comes first.
inline QVariantList available(const QString& surface = QString())
{
    const Layer layer = layerFor(surface);
    if (layer >= LayerCount) {
        return {};
    }
    QMap<QString, QString> files; // sorted by name; the first root wins, as in candidates()
    for (const QString& root : directories(layer)) {
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
        // A file too large isn't read (resolve() says why): its file name.
        if (file.size() <= maxFileSize && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
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
        if (layer == Theme && it.key() == defaultTheme()) {
            list.prepend(entry);
        } else {
            list.append(entry);
        }
    }
    return list;
}

// The pictures of a theme or a style (themes/screenshots/<directory>/, made
// by scripts/theme-pictures.py): <name>.png, or <name>-qs.png and
// <name>-nc.png for quick settings and the notification center, as file
// URLs; the user's (~/.config/vela/themes/screenshots/) before the installed
// ones. None when there is no picture.
inline QStringList pictures(const QString& directory, const QString& name)
{
    QStringList urls;
    const QString dir = QStringLiteral("vela/themes/screenshots/") + directory + u'/';
    for (const QString& file : { name + QStringLiteral(".png"), name + QStringLiteral("-qs.png"), name + QStringLiteral("-nc.png") }) {
        QString path = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, dir + file);
        if (path.isEmpty()) {
            path = QStandardPaths::locate(QStandardPaths::GenericDataLocation, dir + file);
        }
        if (!path.isEmpty()) {
            urls.append(QUrl::fromLocalFile(path).toString());
        }
    }
    return urls;
}

// The installed themes ("") or a surface's styles, with their pictures:
// { name, text, author, images: [file URLs] }. A theme's picture is the
// desktop in it (themes/screenshots/themes/<name>.png); a style's, its
// surface. Those without a picture are listed too, with none.
inline QVariantList gallery(const QString& surface)
{
    QVariantList list;
    const Layer layer = layerFor(surface);
    if (layer >= LayerCount) {
        return list;
    }
    const QString directory = layer == Theme ? QStringLiteral("themes") : surface;
    for (const QVariant& entry : available(surface)) {
        QVariantMap item = entry.toMap();
        item.insert(QStringLiteral("images"), pictures(directory, item.value(QStringLiteral("name")).toString()));
        list.append(item);
    }
    return list;
}

// A surface as a theme draws it, without styles (themes/screenshots/themes/
// <theme>-<surface>.png, -qs and -nc for "notifications"): what "From the
// theme" shows.
inline QStringList themeSurfacePictures(const QString& theme, const QString& surface)
{
    if (!validName(theme) || layerFor(surface) == Theme || layerFor(surface) >= LayerCount) {
        return {};
    }
    return pictures(QStringLiteral("themes"), theme + u'-' + surface);
}

} // namespace vela::themes
