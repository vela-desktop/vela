// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Taskbar.Test

Item {
    width: 760
    height: 700

    ListModel { id: history }
    Component { id: centerComponent; NotificationCenter {} }

    TestCase {
        name: "NotificationClock"
        when: windowShown

        function cleanup() { history.clear() }

        function write(hours, seconds) {
            verify(SettingsFile.writeShellConfig("[clock]\nhours=" + hours
                + "\n[taskbar]\nclockSeconds=" + seconds + "\n"))
        }

        function check(card, expected) {
            const time = findChild(card, "notificationTime")
            const summary = findChild(card, "notificationSummary")
            verify(time !== null)
            verify(summary !== null)
            tryCompare(time, "text", expected)
            verify(time.visible)
            tryVerify(() => time.mapToItem(card, 0, 0).x >= 0)
            tryVerify(() => time.mapToItem(card, time.width, 0).x <= card.width)
            tryVerify(() => summary.mapToItem(card, summary.width, 0).x <= time.mapToItem(card, 0, 0).x)
        }

        function test_live_format_round_trip_data() {
            return [
                { tag: "midnight", hour: 0, minute: 5, hour24: "00:05", hour12: "12:05 AM" },
                { tag: "noon", hour: 12, minute: 5, hour24: "12:05", hour12: "12:05 PM" },
                { tag: "afternoon", hour: 13, minute: 45, hour24: "13:45", hour12: "1:45 PM" },
                { tag: "before-midnight", hour: 23, minute: 59, hour24: "23:59", hour12: "11:59 PM" }
            ]
        }

        function test_live_format_round_trip(data) {
            write(24, false)
            tryCompare(Config, "timeFormat", "HH:mm")
            Notifications.history = history
            history.append({ notificationId: 1, appName: "Clock test", icon: "",
                summary: "The notification keeps its time while the format changes",
                body: "", actions: [], hasDefaultAction: false,
                time: new Date(2026, 10, 15, data.hour, data.minute, 7) })
            const center = createTemporaryObject(centerComponent, null)
            verify(center)
            center.open()
            tryCompare(center, "visible", true)
            tryVerify(() => findChild(center.contentItem, "notificationCard") !== null)
            const card = findChild(center.contentItem, "notificationCard")
            check(card, data.hour24)
            const before = findChild(card, "notificationTime").width
            // Keep the actual center, list and delegate alive. The taskbar's
            // seconds setting must not add seconds to notification timestamps.
            for (let i = 0; i < 3; ++i) {
                write(12, i % 2 === 0)
                check(card, data.hour12)
                write(24, i % 2 !== 0)
                check(card, data.hour24)
                tryCompare(findChild(card, "notificationTime"), "width", before)
                compare(findChild(center.contentItem, "notificationCard"), card)
                compare(history.count, 1)
            }
            center.close()
            tryCompare(center, "visible", false)
        }
    }
}
