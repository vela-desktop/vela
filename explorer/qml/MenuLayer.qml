// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls
import "menufilter.js" as MenuFilter

// The right-click menus inside Explorer's window, with the same panel as the
// desktop (MenuPanel.qml, docs/renderer.md §14): it covers the window, menus
// and submenus are panels inside it, and a click outside closes them.
// open(items, x, y) with x and y in the window.
Item {
    id: root
    visible: false

    property var panels: []
    property var options: ({})
    property var pending: null
    readonly property bool isOpen: visible

    Component {
        id: panelComponent
        MenuPanel { fill: Theme.flyout }
    }

    function open(entries, x, y, opts) {
        closeAll(false, true)
        // Every entry hidden in Settings: no menu, and no layer taking the
        // next click.
        if (MenuFilter.visible(entries, Style.hiddenMenuEntries).length === 0) {
            closeAll(false)
            if (opts && opts.onClosed) {
                opts.onClosed()
            }
            return
        }
        options = opts || {}
        visible = true
        openLevel(0, entries, {
            anchorX: x, anchorY: y,
            above: !!options.above, centered: !!options.centered,
            minWidth: options.minWidth || 0,
            keyboardMode: !!options.keyboard
        }, !!options.keyboard)
    }

    function openLevel(level, entries, properties, selectFirst) {
        closeFrom(level)
        const panel = panelComponent.createObject(root, Object.assign({ entries: entries, level: level }, properties))
        panel.activated.connect(entry => {
            root.closeAll(true)
            if (entry.action) {
                Qt.callLater(entry.action)
            }
        })
        panel.hovered.connect((index, rowY) => root.onHovered(panel, index, rowY))
        panel.submenuRequested.connect((index, rowY, keyboard) => root.openSubmenu(panel, index, rowY, keyboard))
        panel.contextRequested.connect((entry, px, py) => {
            root.openLevel(panel.level + 1, entry.context, { anchorX: px, anchorY: py, keyboardMode: panel.keyboardMode }, false)
        })
        panel.backRequested.connect(() => {
            if (panel.level > 0) {
                root.closeFrom(panel.level)
                root.panels[panel.level - 1].forceActiveFocus()
            } else {
                root.closeAll(true)
            }
        })
        panel.closeAllRequested.connect(() => root.closeAll(true))
        // Its rows changed while open (an entry hidden or shown again): a
        // submenu or a pending hover may point at a row that moved.
        panel.layoutChanged.connect(() => {
            if (root.pending && root.pending.panel === panel) {
                root.pending = null
            }
            if (root.panels.length > panel.level + 1) {
                root.closeFrom(panel.level + 1)
                panel.forceActiveFocus()
            }
        })
        if (selectFirst) {
            panel.move(1)
        }
        panels.push(panel)
        panel.forceActiveFocus()
        return panel
    }

    function openSubmenu(parentPanel, index, rowY, keyboard) {
        pending = null
        const child = panels[parentPanel.level + 1]
        if (child && child.parentIndex === index) {
            return
        }
        const panel = openLevel(parentPanel.level + 1, parentPanel.visibleEntries[index].children, {
            anchorX: parentPanel.x + parentPanel.width - 4,
            altX: parentPanel.x + 4,
            anchorY: parentPanel.y + rowY - 4,
            keyboardMode: parentPanel.keyboardMode
        }, keyboard)
        panel.parentIndex = index
        parentPanel.currentIndex = index
    }

    // Like Windows: submenus open (and close) after a short pause.
    function onHovered(panel, index, rowY) {
        const entry = panel.visibleEntries[index]
        const child = panels[panel.level + 1]
        if (child && child.parentIndex === index) {
            pending = null
            return
        }
        if (entry.children && entry.enabled !== false) {
            pending = { panel: panel, index: index, rowY: rowY }
        } else if (child) {
            pending = { panel: panel, index: -1 }
        } else {
            pending = null
        }
        if (panel.level > 0) {
            panels[panel.level - 1].currentIndex = panel.parentIndex
        }
        if (pending) {
            hoverTimer.restart()
        }
    }

    Timer {
        id: hoverTimer
        interval: 400
        onTriggered: {
            const p = root.pending
            root.pending = null
            if (!p || root.panels.indexOf(p.panel) < 0) {
                return
            }
            if (p.index >= 0) {
                root.openSubmenu(p.panel, p.index, p.rowY, false)
            } else {
                root.closeFrom(p.panel.level + 1)
                p.panel.forceActiveFocus()
            }
        }
    }

    function closeFrom(level) {
        while (panels.length > level) {
            panels.pop().destroy()
        }
    }

    function closeAll(notify, reopening) {
        const wasOpen = visible
        pending = null
        closeFrom(0)
        if (reopening) {
            return
        }
        visible = false
        if (wasOpen && notify && options.onClosed) {
            options.onClosed()
        }
    }

    // A click outside the menus closes them.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: root.closeAll(true)
        onWheel: {}
    }
}
