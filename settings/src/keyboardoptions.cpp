// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keyboardoptions.h"

#include <QRegularExpression>

#include <algorithm>

namespace vela::keyboardoptions {

namespace {

struct Choice {
    const char* value;
    const char* option; // nullptr: the default, no option
};

// What each control offers, its default first.
QList<Choice> choices(Control control)
{
    switch (control) {
    case Control::Caps:
        return { { "capslock", nullptr }, { "ctrl", "ctrl:nocaps" }, { "escape", "caps:escape" },
            { "backspace", "caps:backspace" }, { "off", "caps:none" } };
    case Control::Compose:
        return { { "none", nullptr }, { "ralt", "compose:ralt" }, { "rwin", "compose:rwin" },
            { "menu", "compose:menu" }, { "rctrl", "compose:rctrl" }, { "caps", "compose:caps" },
            { "sclk", "compose:sclk" } };
    case Control::Corner:
        return { { "standard", nullptr }, { "ctrl-alt", "ctrl:swap_lalt_lctl" },
            { "alt-win", "altwin:swap_lalt_lwin" } };
    case Control::Numpad: return { { "default", nullptr }, { "digits", "numpad:mac" } };
    }
    return {};
}

// Whether `option` changes what the control is about.
bool ofKind(const QString& option, Control control)
{
    switch (control) {
    case Control::Caps:
        return option.startsWith(QLatin1String("caps:")) || option == QLatin1String("ctrl:nocaps")
            || option == QLatin1String("ctrl:swapcaps") || option == QLatin1String("ctrl:grouptoggle_capscontrol")
            || option == QLatin1String("ctrl:hyper_capscontrol");
    case Control::Compose: return option.startsWith(QLatin1String("compose:"));
    case Control::Corner:
        return (option.startsWith(QLatin1String("ctrl:swap_")) && option != QLatin1String("ctrl:swapcaps"))
            || option.startsWith(QLatin1String("altwin:swap_")) || option == QLatin1String("altwin:ctrl_alt_win")
            || option == QLatin1String("altwin:alt_win") || option == QLatin1String("altwin:ctrl_win")
            || option == QLatin1String("altwin:ctrl_rwin");
    case Control::Numpad: return option.startsWith(QLatin1String("numpad:"));
    }
    return false;
}

} // namespace

QStringList split(const QString& options)
{
    QStringList list;
    for (const QString& option : options.split(u',', Qt::SkipEmptyParts)) {
        const QString trimmed = option.trimmed();
        if (!trimmed.isEmpty()) {
            list << trimmed;
        }
    }
    return list;
}

QStringList fromLocaled(const QString& text)
{
    // Option "XkbOptions" "caps:escape", as the compositor reads it.
    static const QRegularExpression option(QStringLiteral(R"re(^\s*Option\s+"XkbOptions"\s+"([^"]*)")re"));
    QStringList options;
    for (const QString& line : text.split(u'\n')) {
        const QRegularExpressionMatch match = option.match(line);
        if (match.hasMatch()) {
            options = split(match.captured(1));
        }
    }
    return options;
}

QString value(const QStringList& options, Control control)
{
    const QList<Choice> offered = choices(control);
    QStringList found;
    for (const QString& option : options) {
        if (ofKind(option, control)) {
            found << option;
        }
    }
    if (found.isEmpty() || found == QStringList { QStringLiteral("caps:capslock") }) {
        return QString::fromLatin1(offered.first().value);
    }
    if (found.size() == 1) {
        for (const Choice& choice : offered) {
            if (choice.option && found.first() == QLatin1String(choice.option)) {
                return QString::fromLatin1(choice.value);
            }
        }
    }
    return QStringLiteral("custom");
}

QStringList withValue(const QStringList& options, Control control, const QString& value)
{
    const QList<Choice> offered = choices(control);
    const auto chosen = std::find_if(
        offered.cbegin(), offered.cend(), [&](const Choice& choice) { return value == QLatin1String(choice.value); });
    if (chosen == offered.cend()) {
        return options;
    }
    // Compose on Caps Lock: the key can't do both.
    const bool composeOnCaps = control == Control::Compose && value == QLatin1String("caps");
    QStringList result;
    for (const QString& option : options) {
        if (!ofKind(option, control) && !(composeOnCaps && ofKind(option, Control::Caps))) {
            result << option;
        }
    }
    if (chosen->option) {
        result << QString::fromLatin1(chosen->option);
    }
    return result;
}

} // namespace vela::keyboardoptions
