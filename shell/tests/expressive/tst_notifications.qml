// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../../qml"
import Vela.Shell.Test as ShellTest
import "../../../common/tests/colors.js" as Colors
import "../notifications.js" as Data

// Notifications under the expressive variants (notifications-variant and
// controls-variant=expressive, the shell-controls-expressive test theme):
// the toasts' tonal surface, large corners and error outline, and the
// notification center's runs of one app as one container, M3's segmented
// list: the first card's top corners and the last one's bottom corners the
// outer ones, those between extra small, 2 apart; a run of one is round.
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

    Component {
        id: toastComponent
        NotificationToast {
            width: 356
            notificationId: 7
            appName: "Mail"
            icon: ""
            summary: "Ada Lovelace"
            body: "The engine's notes"
            actions: []
            hasDefaultAction: true
            critical: false
        }
    }

    TestCase {
        name: "NotificationsExpressive"
        when: windowShown

        function init() {
            if (!Theme.expressive || !root.style.expressive)
                skip("with the expressive theme (the *-expressive ctest entries)")
            mouseMove(root, 5, 690)
            history.clear()
            for (const n of Data.history())
                history.append(n)
            waitForRendering(list)
        }

        function corners(card) {
            return [card.topLeftRadius, card.topRightRadius, card.bottomLeftRadius, card.bottomRightRadius]
        }

        function test_toast_is_tonal_with_large_corners() {
            const t = createTemporaryObject(toastComponent, root)
            compare(t.radius, Theme.shapeLarge)
            compare(t.border.width, 0, "no outline")
            verify(Colors.same(t.color, Theme.popup))
            verify(Colors.same(Theme.popup, Theme.surfaceContainerHigh))
            const critical = createTemporaryObject(toastComponent, root, { critical: true, y: 200 })
            compare(critical.border.width, 1)
            verify(Colors.same(critical.border.color, Theme.error))
            tryCompare(t, "opacity", 1, 5000)
            tryCompare(t.transform[0], "x", 0, 5000)
        }

        function test_one_apps_run_is_one_container() {
            const c = Data.cards(list)
            const outer = style.cardRadius
            const inner = Math.min(outer, Theme.shapeExtraSmall)
            compare(outer, Theme.shapeLarge)
            compare(inner, 4)
            compare(corners(c[1]), [outer, outer, inner, inner], "the first: the outer corners on top")
            compare(corners(c[2]), [inner, inner, inner, inner], "between: extra small")
            compare(corners(c[3]), [inner, inner, outer, outer], "the last: the outer corners below")
            compare(corners(c[4]), [outer, outer, outer, outer], "a run of one is round")
            compare(corners(c[5]), [outer, outer, inner, inner])
            compare(corners(c[6]), [inner, inner, outer, outer])
            compare(list.spacing, 2)
            compare(Data.gap(c[1], c[2]), 2)
            compare(Data.gap(c[2], c[3]), 2)
            compare(Data.gap(c[5], c[6]), 2)
            verify(Data.gap(c[3], c[4]) > 2 + 10, "the next app's name between two runs")
        }

        function test_a_run_changes_with_the_history() {
            // Grace's dismissed: Ada and Linus still one run.
            history.remove(1)
            let c = Data.cards(list)
            const outer = style.cardRadius
            const inner = Math.min(outer, Theme.shapeExtraSmall)
            tryVerify(() => Data.cards(list)[3].topLeftRadius === inner, 5000)
            c = Data.cards(list)
            compare(corners(c[1]), [outer, outer, inner, inner])
            compare(corners(c[3]), [inner, inner, outer, outer])
            // Linus too: Ada alone, round.
            history.remove(1)
            tryVerify(() => Data.cards(list)[1].bottomLeftRadius === outer, 5000)
            compare(corners(Data.cards(list)[1]), [outer, outer, outer, outer])
        }

        function test_card_hover_and_summary() {
            const c = Data.cards(list)
            verify(Colors.same(c[1].color, Theme.control))
            mouseMove(c[1], 100, 10)
            tryVerify(() => Colors.same(c[1].color, Theme.controlHover), 5000)
            const summary = walk(c[1], x => x.text === "Ada" && x.font !== undefined)
            compare(summary.font.weight, Theme.weightEmphasis)
        }

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
    }
}
