// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 lowell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtLogging>

#include <array>

// Theme files (docs/configuration.md, "Theme files"): the values Theme.qml
// takes over Vela's own. Two kinds, like Windhawk's theme and its three
// stylers:
//
//   theme=<name>                  themes/<name>.conf: colors and radii of the
//                                 shell and the apps
//   taskbar-style=<name>          themes/taskbar/<name>.conf
//   startmenu-style=<name>        themes/startmenu/<name>.conf
//   notifications-style=<name>    themes/notifications/<name>.conf (quick
//                                 settings and the notification center)
//
// The first comment line names the file ("# Rosé Pine: ...") and a
// "# Original author: <name>" line among the first comments credits it.
// Vela's own look is a theme too, themes/vela.conf ("Vela Official"), and
// the one used when theme= is empty or missing.
//
// `key=value` lines and `#` comments. A key is a Theme.qml token,
// "dark.<token>" or "light.<token>" for one mode only. A value is a color
// (#RRGGBB, #AARRGGBB), a number, or asset:<file>, a picture in
// themes/assets/. A style's values win over the theme's.
//
// Every file is looked up in the user's ~/.config/vela/ first, then in the
// data directories (a package, or a nested run's XDG_DATA_DIRS).
namespace vela::themes {

// The layers, in the order they apply. The surfaces' styles only set keys of
// their own surface (a test checks it), so their order among themselves
// doesn't matter.
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

// The theme when vela.conf chooses none: Vela's own (themes/vela.conf).
inline QString defaultTheme()
{
    return QStringLiteral("vela");
}

// A name is a file name without ".conf": nothing that leads elsewhere.
inline bool validName(const QString& name)
{
    static const QRegularExpression safeName(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$"));
    return safeName.match(name).hasMatch();
}

// The chosen names in a vela.conf, by layer ("" where none, but the theme:
// defaultTheme()); the last line wins if a key is there twice (like the
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

inline QString configuredName(const QString& velaConf)
{
    return configured(velaConf)[Theme];
}

// The file of a theme or style, or empty when the name is not valid or not
// installed.
inline QString locate(const QString& name, Layer layer = Theme)
{
    if (!validName(name) || layer >= LayerCount) {
        return {};
    }
    const QString relative = subdirectory(layer) + u'/' + name + QStringLiteral(".conf");
    const QString user = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, relative);
    return user.isEmpty() ? QStandardPaths::locate(QStandardPaths::GenericDataLocation, relative) : user;
}

// asset:<file>: a picture of themes/assets/ (the user's first), as a file
// URL; empty if it is not installed. Pictures are not shipped (Windows'
// own, for Aero): without them the drawn look stays.
inline QString assetUrl(const QString& name)
{
    if (name.isEmpty() || name.startsWith(u'/') || name.contains(QLatin1String(".."))) {
        return {};
    }
    const QString relative = QStringLiteral("vela/themes/assets/") + name;
    QString file = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, relative);
    if (file.isEmpty()) {
        file = QStandardPaths::locate(QStandardPaths::GenericDataLocation, relative);
    }
    return file.isEmpty() ? QString() : QUrl::fromLocalFile(file).toString();
}

// Only identifiers of built-in Vela implementations may be selected by theme
// files. No arbitrary QML paths, scripts, or extension code can be loaded.
inline bool allowedVariant(const QString& key, const QString& value)
{
    static const QMap<QString, QStringList> variants {
        { QStringLiteral("taskbar-variant"), { QStringLiteral("default"), QStringLiteral("floating"), QStringLiteral("floating-split"), QStringLiteral("dock") } },
        { QStringLiteral("startmenu-variant"), { QStringLiteral("default"), QStringLiteral("seven") } },
        { QStringLiteral("quicksettings-variant"), { QStringLiteral("default"), QStringLiteral("glass"), QStringLiteral("classic") } },
        { QStringLiteral("notifications-variant"), { QStringLiteral("default"), QStringLiteral("aero") } },
    };
    return variants.contains(key) && variants.value(key).contains(value);
}

// The values of a file: a number as double, a color as QColor, a picture as
// its URL (an empty string when not installed). A line that is none of these
// is skipped with a warning (`origin` names the file in it).
inline QVariantMap parse(QIODevice& in, const QString& origin = QString())
{
    QVariantMap map;
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
        bool isNumber = false;
        const double d = value.toDouble(&isNumber);
        const QString bareKey = key.startsWith(QLatin1String("dark.")) || key.startsWith(QLatin1String("light."))
            ? key.section(u'.', 1) : key;
        if (key.isEmpty() || value.isEmpty()) {
            qWarning("theme %s:%d: not a key=value line", qPrintable(origin), number);
        } else if (key == QLatin1String("inherits")) {
            if (validName(value)) {
                map.insert(key, value);
            } else {
                qWarning("theme %s:%d: invalid parent theme", qPrintable(origin), number);
            }
        } else if (bareKey.endsWith(QLatin1String("-variant"))) {
            if (allowedVariant(bareKey, value)) {
                map.insert(key, value);
            } else {
                qWarning("theme %s:%d: unsupported official variant %s=%s", qPrintable(origin), number,
                    qPrintable(key), qPrintable(value));
            }
        } else if (isNumber) {
            map.insert(key, d);
        } else if (value.startsWith(u'#') && QColor::isValidColorName(value)) {
            map.insert(key, QColor::fromString(value));
        } else if (value.startsWith(QLatin1String("asset:"))) {
            map.insert(key, assetUrl(value.mid(6).trimmed()));
        } else {
            qWarning("theme %s:%d: %s is neither a color, a number nor an asset:", qPrintable(origin), number,
                qPrintable(key));
        }
    }
    return map;
}

inline QVariantMap load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return parse(file, path);
}

// Resolve the one-parent inheritance chain within the same theme/style family.
// Cycles, inaccessible parents and excessive depth cannot trap the shell.
inline QVariantMap loadInherited(const QString& name, Layer layer = Theme, QSet<QString>* ancestors = nullptr, int depth = 0)
{
    if (!validName(name) || depth >= 16) {
        qWarning("theme: invalid name or inheritance depth");
        return {};
    }
    QSet<QString> stack;
    if (!ancestors) {
        ancestors = &stack;
    }
    if (ancestors->contains(name)) {
        qWarning("theme: inheritance cycle at %s", qPrintable(name));
        return {};
    }
    const QString path = locate(name, layer);
    if (path.isEmpty()) {
        qWarning("theme: missing inherited package %s", qPrintable(name));
        return {};
    }
    ancestors->insert(name);
    QVariantMap own = load(path);
    const QString parent = own.take(QStringLiteral("inherits")).toString();
    const QVariant version = own.take(QStringLiteral("schema-version"));
    if (version.isValid() && version.toInt() != 1) {
        qWarning("theme: unsupported schema in %s", qPrintable(path));
        ancestors->remove(name);
        return {};
    }
    QVariantMap resolved;
    if (!parent.isEmpty()) {
        resolved = loadInherited(parent, layer, ancestors, depth + 1);
    }
    resolved.insert(own);
    ancestors->remove(name);
    return resolved;
}

// A token's value as Theme.qml's themed() finds it: "dark.<key>" or
// "light.<key>" for the mode, else "<key>"; invalid when the map lacks it.
inline QVariant themed(const QVariantMap& theme, const QString& key, bool light)
{
    const QString modeKey = (light ? QStringLiteral("light.") : QStringLiteral("dark.")) + key;
    return theme.contains(modeKey) ? theme.value(modeKey) : theme.value(key);
}

// The chosen theme and styles, each layer on its own (a panel's blur look
// comes from the theme and its own style only) and merged (Theme.qml's).
struct Loaded {
    std::array<QString, LayerCount> files;
    std::array<QVariantMap, LayerCount> layers;

    QVariantMap merged() const
    {
        QVariantMap map;
        for (const QVariantMap& layer : layers) {
            map.insert(layer);
        }
        return map;
    }
    // The theme with just one surface's style.
    QVariantMap withStyle(Layer layer) const
    {
        QVariantMap map = layers[Theme];
        if (layer != Theme && layer < LayerCount) {
            map.insert(layers[layer]);
        }
        return map;
    }
};

inline Loaded loadConfigured(const QString& velaConf)
{
    Loaded loaded;
    const Choice choice = configured(velaConf);
    for (int i = 0; i < LayerCount; ++i) {
        loaded.files[i] = locate(choice[i], Layer(i));
        if (!loaded.files[i].isEmpty()) {
            loaded.layers[i] = loadInherited(choice[i], Layer(i));
        }
    }
    return loaded;
}

// The installed themes ("") or a surface's styles ("taskbar"...), by name:
// { name, text, author }. The text is the name the first line gives
// ("# Rosé Pine: ..."), else the file name; the author, the "# Original
// author:" line's ("" without one). The user's win over the installed ones
// of the same name; Vela's own theme comes first.
inline QVariantList available(const QString& surface = QString())
{
    const Layer layer = layerFor(surface);
    if (layer >= LayerCount) {
        return {};
    }
    QStringList roots = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, subdirectory(layer), QStandardPaths::LocateDirectory);
    roots.prepend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u'/' + subdirectory(layer));
    QMap<QString, QString> files; // sorted by name; the first root wins
    for (const QString& root : std::as_const(roots)) {
        for (const QString& file : QDir(root).entryList({ QStringLiteral("*.conf") }, QDir::Files)) {
            if (!files.contains(file.chopped(5))) {
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
        if (layer == Theme && it.key() == defaultTheme()) {
            list.prepend(entry);
        } else {
            list.append(entry);
        }
    }
    return list;
}

// The styles of a surface with their pictures (themes/screenshots/<surface>/
// <name>.png, or <name>-qs.png and <name>-nc.png for quick settings and the
// notification center): { name, text, images: [file URLs] }. Styles without
// a picture are listed too, with none.
inline QVariantList gallery(const QString& surface)
{
    QVariantList list;
    if (layerFor(surface) == Theme || layerFor(surface) >= LayerCount) {
        return list;
    }
    const QString dir = QStringLiteral("vela/themes/screenshots/") + surface + u'/';
    const auto picture = [&dir](const QString& file) {
        QString path = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, dir + file);
        if (path.isEmpty()) {
            path = QStandardPaths::locate(QStandardPaths::GenericDataLocation, dir + file);
        }
        return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
    };
    for (const QVariant& entry : available(surface)) {
        QVariantMap style = entry.toMap();
        const QString name = style.value(QStringLiteral("name")).toString();
        QStringList images;
        for (const QString& file : { name + QStringLiteral(".png"), name + QStringLiteral("-qs.png"), name + QStringLiteral("-nc.png") }) {
            if (const QString url = picture(file); !url.isEmpty()) {
                images.append(url);
            }
        }
        style.insert(QStringLiteral("images"), images);
        list.append(style);
    }
    return list;
}

} // namespace vela::themes
