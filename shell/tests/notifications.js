// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The notification tests' data: a history with runs of one app's
// notifications (three, one, two), as the notification center lists them.
.pragma library

function history() {
    const at = (h, m) => new Date(2026, 10, 12, h, m)
    return [
        { notificationId: 1, appName: "Mail", icon: "", summary: "Ada", body: "The draft", actions: [], hasDefaultAction: true, time: at(9, 30) },
        { notificationId: 2, appName: "Mail", icon: "", summary: "Grace", body: "", actions: [], hasDefaultAction: false, time: at(9, 31) },
        { notificationId: 3, appName: "Mail", icon: "", summary: "Linus", body: "Patches", actions: [], hasDefaultAction: true, time: at(9, 32) },
        { notificationId: 4, appName: "Calendar", icon: "", summary: "Stand-up", body: "In 10 minutes", actions: [], hasDefaultAction: true, time: at(9, 50) },
        { notificationId: 5, appName: "Files", icon: "", summary: "Copied", body: "3 files", actions: [], hasDefaultAction: false, time: at(10, 0) },
        { notificationId: 6, appName: "Files", icon: "", summary: "Moved", body: "1 file", actions: [], hasDefaultAction: false, time: at(10, 1) }
    ]
}

// The cards of `list` (NotificationList), by notification id.
function cards(list) {
    const found = {}
    const items = list.contentItem.children
    for (let i = 0; i < items.length; ++i) {
        if (items[i].objectName === "notificationCard")
            found[items[i].notificationId] = items[i]
    }
    return found
}

// The gap between two cards, one above the other.
function gap(upper, lower) {
    return lower.y - (upper.y + upper.height)
}
