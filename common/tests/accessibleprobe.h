// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What a screen reader is told about an item: its QAccessibleInterface, not
// the attached Accessible properties, which Qt's controls may overwrite or
// leave out once accessibility is active. Registered by the Qt Quick Test
// runners as the singleton `AccessibleProbe` of `Vela.Test` 1.0:
//
//     const state = AccessibleProbe.state(item)
//     compare(state.role, Accessible.CheckBox); verify(state.checked)

#include <QAccessible>
#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

namespace vela::test {

class AccessibleProbe : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    /** `item`'s role, name, description and states as its interface reports them. */
    Q_INVOKABLE QVariantMap state(QObject* item) const
    {
        QAccessibleInterface* iface = item ? QAccessible::queryAccessibleInterface(item) : nullptr;
        if (!iface || !iface->isValid()) {
            return { { QStringLiteral("valid"), false } };
        }
        const QAccessible::State st = iface->state();
        return {
            { QStringLiteral("valid"), true },
            { QStringLiteral("role"), int(iface->role()) },
            { QStringLiteral("name"), iface->text(QAccessible::Name) },
            { QStringLiteral("description"), iface->text(QAccessible::Description) },
            { QStringLiteral("checkable"), bool(st.checkable) },
            { QStringLiteral("checked"), bool(st.checked) },
            { QStringLiteral("pressed"), bool(st.pressed) },
            { QStringLiteral("focusable"), bool(st.focusable) },
            { QStringLiteral("disabled"), bool(st.disabled) },
            { QStringLiteral("invisible"), bool(st.invisible) },
        };
    }

    /** Turn accessibility off and on, as a screen reader starting after the program does. */
    Q_INVOKABLE void restart() const
    {
        QAccessible::setActive(false);
        QAccessible::setActive(true);
    }

    /** Register the singleton (call before the tests' engine loads them). */
    static void install()
    {
        qmlRegisterSingletonType<AccessibleProbe>("Vela.Test", 1, 0, "AccessibleProbe",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new AccessibleProbe; });
    }
};

} // namespace vela::test
