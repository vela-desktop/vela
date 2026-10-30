// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Updates
import "../qml"

// System > Updates' "What's new" (docs/updates.md §9.1): Beta's release
// texts, Experimental's first 10 commits and "and N more", only with an
// update, in plain text.
Item {
    id: root
    width: 900
    height: 700

    WhatsNew {
        id: group
        width: 700
    }

    TestCase {
        name: "WhatsNew"
        when: windowShown

        function find(name) { return findChild(group, name) }
        function commits(n) {
            const list = []
            for (let i = 0; i < n; ++i) {
                list.push({ commit: String(i).padStart(40, "c"), summary: "Change " + i })
            }
            return list
        }
        // The texts drawn inside `item`, in order.
        function texts(item) {
            let list = []
            for (const child of item.children) {
                if (child.visible === false) continue
                if (child.text !== undefined && child.textFormat !== undefined && child.text !== "") {
                    compare(child.textFormat, Text.PlainText)
                    list.push(child.text)
                }
                list = list.concat(texts(child))
            }
            return list
        }

        function init() {
            Updates.reset()
        }

        function test_only_with_an_update() {
            Updates.changes = commits(2)
            Updates.changesTotal = 2
            verify(!group.visible)
            Updates.status = "update-available"
            verify(group.visible)
            Updates.changes = []
            verify(!group.visible)
        }

        function test_experimental_commits() {
            Updates.status = "update-available"
            Updates.changes = commits(100)
            Updates.changesTotal = 140
            verify(find("changes").visible)
            verify(!find("notes").visible)
            const shown = texts(find("changes"))
            compare(shown[0], "Change 0")
            compare(shown[1], "ccccccc")
            verify(shown.indexOf("Change 9") >= 0)
            verify(shown.indexOf("Change 10") < 0)
            compare(find("more").text, "and 130 more")
            Updates.changes = commits(3)
            Updates.changesTotal = 3
            verify(!find("more").visible)
        }

        function test_beta_notes() {
            Updates.status = "update-available"
            Updates.channel = "beta"
            Updates.notes = [{ version: "0.1.11", text: "<b>Faster</b> blur" }, { version: "0.1.10", text: "Fixes" }]
            verify(find("notes").visible)
            verify(!find("changes").visible)
            compare(texts(find("notes")), ["Vela 0.1.11", "<b>Faster</b> blur", "Vela 0.1.10", "Fixes"])
        }

        function test_no_daemon() {
            Updates.status = "update-available"
            Updates.changes = commits(1)
            Updates.available = false
            verify(!group.visible)
        }
    }
}
