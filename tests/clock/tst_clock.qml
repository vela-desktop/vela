// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Taskbar.Test

Item {
    width: 1280
    height: 160

    Component { id: barComponent; Taskbar { visible: true; primary: true } }

    TestCase {
        name: "TaskbarClock"
        when: windowShown

        function write(hours, seconds, date) {
            verify(SettingsFile.writeShellConfig("[clock]\nhours=" + hours
                + "\n[taskbar]\nclockSeconds=" + seconds + "\nshowDate=" + date + "\n"))
        }

        function child(bar, name) {
            const item = findChild(bar.contentItem, name)
            verify(item !== null, name)
            return item
        }

        function check(bar, expected, date) {
            const time = child(bar, "taskbarTime")
            const day = child(bar, "taskbarDate")
            const clock = child(bar, "taskbarClock")
            // Expected text is literal: a stale binding or the wrong format
            // cannot pass just because a neighbouring icon changed pixels.
            tryCompare(time, "text", expected)
            tryCompare(day, "visible", date)
            compare(day.text, "15/11/2026")
            tryCompare(clock, "width", Math.max(time.width, date ? day.width : 0) + 16)
            tryVerify(() => time.mapToItem(clock, 0, 0).x >= 0)
            tryVerify(() => time.mapToItem(clock, time.width, time.height).x <= clock.width)
            return clock.width
        }

        function test_live_format_round_trip_data() {
            return [
                { tag: "seconds-with-date", date: true, hours: 24, seconds: true, text: "23:59:59" },
                { tag: "seconds-without-date", date: false, hours: 24, seconds: true, text: "23:59:59" },
                { tag: "12-hour-with-date", date: true, hours: 12, seconds: false, text: "11:59 PM" },
                { tag: "12-hour-without-date", date: false, hours: 12, seconds: false, text: "11:59 PM" },
                { tag: "12-hour-seconds-with-date", date: true, hours: 12, seconds: true, text: "11:59:59 PM" },
                { tag: "12-hour-seconds-without-date", date: false, hours: 12, seconds: true, text: "11:59:59 PM" }
            ]
        }

        function test_live_format_round_trip(data) {
            write(24, false, data.date)
            tryCompare(Config, "clockFormat", "HH:mm")
            tryCompare(Config, "clockDate", data.date)
            const bar = createTemporaryObject(barComponent, null, { width: 1280 })
            verify(bar)
            tryCompare(bar.contentItem, "width", bar.width)
            const before = check(bar, "23:59", data.date)
            // Keep the same taskbar alive across replacements. Repeating the
            // round trip also checks that Config re-arms its file watcher.
            for (let i = 0; i < 3; ++i) {
                write(data.hours, data.seconds, data.date)
                check(bar, data.text, data.date)
                write(24, false, data.date)
                check(bar, "23:59", data.date)
                tryCompare(child(bar, "taskbarClock"), "width", before)
            }
        }
    }
}
