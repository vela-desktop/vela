// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Test
import "../qml"
import Vela.Shell.Test as ShellTest
import "../../common/tests/colors.js" as Colors
import "notifications.js" as Data

// Notifications in the default look: the toasts (NotificationToast.qml) and
// the notification center's list (NotificationList.qml). Their shape and
// colors against the live Theme and Styles, what a click, a right click,
// an action and Close ask for, and what a screen reader hears.
Item {
    id: root
    width: 760
    height: 700

    readonly property QtObject style: ShellTest.Styles.notifications

    ListModel { id: history }
    NotificationList {
        id: list
        x: 380
        width: 360
        height: 700
        style: root.style
        timeFormat: "HH:mm"
        model: history
    }
    SignalSpy { id: listInvoked; target: list; signalName: "invoked" }
    SignalSpy { id: listDismissed; target: list; signalName: "dismissed" }

    Component {
        id: toastComponent
        NotificationToast {
            width: 356
            notificationId: 7
            appName: "Mail"
            icon: ""
            summary: "Ada Lovelace"
            body: "The engine's notes"
            actions: [{ key: "reply", label: "Reply" }, { key: "read", label: "Mark as read" }]
            hasDefaultAction: true
            critical: false
        }
    }

    TestCase {
        name: "Notifications"
        when: windowShown

        function init() {
            mouseMove(root, 5, 690)
            history.clear()
            for (const n of Data.history())
                history.append(n)
            listInvoked.clear()
            listDismissed.clear()
            waitForRendering(list)
        }

        function toast(properties) {
            const t = createTemporaryObject(toastComponent, root, Object.assign({ x: 12, y: 12 }, properties || {}))
            verify(t)
            return t
        }

        // --- toasts ---

        function test_toast_look() {
            verify(!Theme.expressive)
            const t = toast()
            compare(t.radius, Theme.radiusOverlay)
            compare(t.border.width, 1)
            verify(Colors.same(t.border.color, Theme.stroke))
            verify(Colors.same(t.color, Theme.popup))
            const critical = toast({ critical: true, y: 200 })
            compare(critical.border.width, 1)
            verify(Colors.same(critical.border.color, Qt.rgba(1, 0.35, 0.35, 0.6)))
        }

        function test_toast_comes_in_from_the_right() {
            const t = toast()
            const slide = t.transform[0]
            if (Theme.motion > 0)
                verify(slide.x > 0 || t.opacity < 1, "it moves in")
            tryCompare(t, "opacity", 1, 5000)
            tryCompare(slide, "x", 0, 5000)
        }

        function test_toast_click_invokes_or_dismisses() {
            const t = toast()
            const invoked = createTemporaryObject(spy, root, { target: t, signalName: "invoked" })
            const dismissed = createTemporaryObject(spy, root, { target: t, signalName: "dismissed" })
            tryCompare(t, "opacity", 1, 5000)
            mouseClick(t, 200, 20)
            compare(invoked.count, 1)
            compare(invoked.signalArguments[0][0], "default")
            compare(dismissed.count, 0)
            mouseClick(t, 200, 20, Qt.RightButton)
            compare(dismissed.count, 1)
            compare(invoked.count, 1, "a right click invokes nothing")
            const plain = toast({ hasDefaultAction: false, y: 200 })
            const plainDismissed = createTemporaryObject(spy, root, { target: plain, signalName: "dismissed" })
            mouseClick(plain, 200, 20)
            compare(plainDismissed.count, 1, "no default action: a click dismisses")
        }

        function test_toast_actions_and_close() {
            const t = toast()
            const invoked = createTemporaryObject(spy, root, { target: t, signalName: "invoked" })
            const dismissed = createTemporaryObject(spy, root, { target: t, signalName: "dismissed" })
            tryCompare(t, "opacity", 1, 5000)
            const reply = findButton(t, "Reply")
            verify(reply)
            mouseClick(reply)
            compare(invoked.signalArguments[0][0], "reply")
            mouseClick(findButton(t, "Mark as read"))
            compare(invoked.signalArguments[1][0], "read")
            const close = findButton(t, "", "Close")
            verify(close)
            compare(close.opacity, 0)
            mouseMove(t, 200, 20)
            tryCompare(close, "opacity", 1, 5000)
            compare(AccessibleProbe.state(close).name, "Close")
            mouseClick(close)
            compare(dismissed.count, 1)
            compare(invoked.count, 2, "Close invokes nothing")
        }

        function test_toast_texts() {
            const t = toast({ body: "" })
            const texts = textsOf(t)
            verify(texts.some(x => x.text === "Mail" && Colors.same(x.color, Theme.textSecondary)))
            const summary = texts.find(x => x.text === "Ada Lovelace")
            compare(summary.font.weight, Theme.weightEmphasis)
            verify(Colors.same(summary.color, Theme.text))
            verify(!texts.find(x => x.text === "" && x.textFormat === Text.StyledText).visible, "no body, no line")
        }

        // --- the notification center's list ---

        function test_runs_under_their_app() {
            const c = Data.cards(list)
            compare(Object.keys(c).length, 6)
            // The default look: every card the style's corners, all four.
            for (const id in c) {
                compare(c[id].radius, style.cardRadius)
                for (const corner of ["topLeftRadius", "topRightRadius", "bottomLeftRadius", "bottomRightRadius"])
                    compare(c[id][corner], style.cardRadius, `card ${id}: ${corner}`)
            }
            compare(list.spacing, 6)
            compare(Data.gap(c[1], c[2]), 6)
            compare(Data.gap(c[2], c[3]), 6)
            compare(Data.gap(c[5], c[6]), 6)
            // Between two apps, the next one's name.
            verify(Data.gap(c[3], c[4]) > 6 + 10, "Calendar's name between Mail and Calendar")
            verify(c[1].first && !c[1].last)
            verify(!c[2].first && !c[2].last)
            verify(!c[3].first && c[3].last)
            verify(c[4].first && c[4].last)
        }

        function test_card_states_and_actions() {
            const c = Data.cards(list)
            verify(Colors.same(c[1].color, Theme.control))
            mouseMove(c[1], 100, 10)
            tryVerify(() => Colors.same(c[1].color, Theme.subtleHover), 5000)
            mouseClick(c[1], 100, 10)
            compare(listInvoked.count, 1)
            compare(listInvoked.signalArguments[0][0], 1)
            compare(listInvoked.signalArguments[0][1], "default")
            mouseClick(c[2], 100, 10)
            compare(listInvoked.count, 1, "no default action: a click does nothing")
            compare(listDismissed.count, 0)
            mouseClick(c[2], 100, 10, Qt.RightButton)
            compare(listDismissed.count, 1)
            compare(listDismissed.signalArguments[0][0], 2)
            // Close, with the mouse over the card.
            mouseMove(c[3], 100, 10)
            const close = findButton(c[3], "", "")
            tryVerify(() => close.visible, 5000)
            mouseClick(close)
            compare(listDismissed.count, 2)
            compare(listDismissed.signalArguments[1][0], 3)
        }

        function test_card_texts_and_accessibility() {
            const c = Data.cards(list)
            const texts = textsOf(c[1])
            verify(texts.some(x => x.text === "09:30"), "the time, in the list's format")
            compare(texts.find(x => x.text === "Ada").font.weight, Font.DemiBold)
            const state = AccessibleProbe.state(c[1])
            compare(state.role, Accessible.ListItem)
            compare(state.name, "Ada")
            compare(state.description, "The draft")
        }

        function test_empty_list() {
            history.clear()
            const empty = textsOf(list).find(x => x.text === "No new notifications")
            tryVerify(() => empty.visible, 5000)
            history.append(Data.history()[0])
            tryVerify(() => !empty.visible, 5000)
        }

        // --- helpers ---

        function walk(item, test) {
            if (test(item))
                return item
            for (let i = 0; i < item.children.length; ++i) {
                const found = walk(item.children[i], test)
                if (found)
                    return found
            }
            return null
        }
        function findButton(item, text, name) {
            return walk(item, x => x.clicked !== undefined && x.text === text
                && (name === undefined || x.Accessible.name === name))
        }
        function textsOf(item) {
            const out = []
            walk(item, x => { if (x.textFormat !== undefined && x.text !== undefined && x.font !== undefined) out.push(x); return false })
            return out
        }
    }
    Component { id: spy; SignalSpy {} }
}
