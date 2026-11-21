// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

// The key changes Settings > Keyboard > Typing offers (docs/keyboard.md §6),
// read from and written to the XKB options of `keyboard-options`. Each
// control owns one kind of option; the options of other kinds are kept as
// they are.
namespace vela::keyboardoptions {

enum class Control {
    Caps, // the Caps Lock key: "capslock", "ctrl", "escape", "backspace", "off"
    Compose, // the Compose key: "none", "ralt", "rwin", "menu", "rctrl", "caps", "sclk"
    Corner, // the bottom-left keys: "standard", "ctrl-alt", "alt-win"
    Numpad, // the numeric keypad: "default", "digits"
};

// The control's value in `options`; "custom" when an option of its kind is
// one it doesn't offer, or there are several.
QString value(const QStringList& options, Control control);

// `options` with the control set to `value`: its kind's options replaced by
// the one for `value` (none for the default). Compose on Caps Lock also
// takes the Caps Lock key's options away. An unknown value changes nothing.
QStringList withValue(const QStringList& options, Control control, const QString& value);

// "a,b, c" → ["a", "b", "c"].
QStringList split(const QString& options);

// The options of systemd-localed's 00-keyboard.conf (Option "XkbOptions"),
// what the compositor uses when vela.conf has no keyboard-options.
QStringList fromLocaled(const QString& text);

} // namespace vela::keyboardoptions
