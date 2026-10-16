// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T

// A row of a list (a network, a sound output, an option): Qt's item delegate,
// a click, Space or Enter activate it. Subtle at rest; `highlighted`, the
// chosen one, takes the control's colors; pressed over hovered over rest,
// faded (Fill). Its content is its children, placed by the caller.
T.ItemDelegate {
    id: control
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    background: Fill {
        color: control.highlighted ? (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
             : (control.down ? Theme.subtlePressed : control.hovered ? Theme.subtleHover : Theme.subtleRest)
        FocusRing { visible: control.visualFocus }
    }
}
