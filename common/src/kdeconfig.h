// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// KDE's configuration files (kdeglobals, kcminputrc), read and written as
// KConfig does, without KDE: Vela shares the system font, the icon theme and
// the cursor theme with KDE and Qt apps through them, and works the same
// whether Plasma (or kwriteconfig6) is installed or not
// (docs/system-appearance.md).
//
// QSettings is not a KConfig reader: it maps [General] to its own top-level
// keys, reads "Noto Sans,10,-1,5,400" as a list and writes values with commas
// in quotes that KDE then keeps. These functions edit the file line by line
// and leave every other line as it was.

#include <QList>
#include <QString>

#include <optional>

namespace vela::kde {

// $XDG_CONFIG_HOME/<file> (~/.config/<file>).
QString configPath(const QString& file);

// The value of `key` in `[group]`, as KConfig reads it: escapes (\s, \t, \n,
// \\, \xNN) resolved, the last line of the key in the file winning, keys with
// a locale ("key[it]") left out. nullopt when the file, the group or the key
// is missing.
std::optional<QString> read(const QString& path, const QString& group, const QString& key);

struct Entry {
    QString group;
    QString key;
    QString value;
};

// Sets every entry in one atomic replacement of the file (created when
// missing), under KConfig's own lock (<file>.lock). Existing lines of a key
// are rewritten in place, a missing key goes at the end of its group, a
// missing group at the end of the file. false, with the file untouched, when
// it can't be read or written, or when KDE marks a group or key immutable
// ([$i]).
bool write(const QString& path, const QList<Entry>& entries);

// What `kwriteconfig6 --notify` sends after writing: KConfigWatcher's
// org.kde.kconfig.notify.ConfigChanged for those keys of that file, on the
// session bus.
void notifyChanged(const QString& path, const QList<Entry>& entries);

// KDE's session-wide change messages, for running KDE/Qt apps:
// KGlobalSettings.notifyChange (KDE's platform plugin rereads the icon theme
// on IconChanged), KDEPlatformTheme.refreshFonts (it rereads the fonts) and
// KIconLoader.iconChanged (KIconLoader empties its cache).
enum class GlobalChange {
    Font = 1,
    Icon = 4,
    Cursor = 5,
};
void notifyGlobalChange(GlobalChange change);
void refreshFonts();
void iconLoaderChanged();

// A value as KConfig writes it (the reverse of what read() resolves).
QString escape(const QString& value);
QString unescape(const QString& value);

} // namespace vela::kde
