// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QVariantList>

// The keyboard layouts XKB knows (evdev.xml), for "Add a keyboard": [{layout,
// variant, name}], sorted by name. Read the first time they're needed.
class KeyboardLayouts : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    Q_INVOKABLE QVariantList all();
    // The name to show ("Italian", "English (US, intl.)...").
    Q_INVOKABLE QString name(const QString& layout, const QString& variant);

    // Where the layout in use comes from when Settings has none, in the
    // compositor's order (compositor/src/keyboard.c, docs/keyboard.md §5.5):
    // {source: "environment" | "system" | "default", layouts: [{layout,
    // variant}]}. "environment": XKB_DEFAULT_LAYOUT is set, and wins even
    // over the layouts chosen in Settings.
    Q_INVOKABLE QVariantMap source() const;
    // The same from given values, for the tests: XKB_DEFAULT_LAYOUT and
    // XKB_DEFAULT_VARIANT, and the text of systemd-localed's
    // 00-keyboard.conf.
    static QVariantMap sourceFrom(const QString& envLayout, const QString& envVariant, const QString& localed);

private:
    void load();
    QVariantList m_all;
};
