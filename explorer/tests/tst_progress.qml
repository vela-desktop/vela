// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Regression: long-running jobs must remain on-screen while the backend copies.
Item {
    id: root
    width: 700
    height: 500

    ProgressPanel {
        id: panel
        anchors { right: parent.right; bottom: parent.bottom; margins: 16 }
    }

    TestCase {
        name: "TransferProgress"
        when: windowShown

        function init() { Ops.jobs = [] }
        function cleanup() { Ops.jobs = [] }

        function job(id, done, total, finished, error) {
            return { id: id, title: "Copying videos", doneTitle: "Copied videos",
                     done: done, total: total, current: "video.mkv",
                     finished: finished, error: error || "" }
        }

        function test_busy_transfer_is_visible_and_cancellable() {
            Ops.jobs = [job(51, 500, 1000, false, "")]
            tryVerify(() => panel.visible && panel.height >= panel.cardHeight)
            const card = findChild(panel, "transferJob:51")
            verify(card !== null, "copy must show a visible card")
            compare(card.fraction, 0.5)
            const cancel = findChild(panel, "transferCancelButton:51")
            verify(cancel !== null)
            mouseClick(cancel)
            compare(Ops.lastCancelled, 51)
        }

        function test_long_job_list_is_clipped_to_window() {
            Ops.jobs = [job(1, 0, 1000, false, ""), job(2, 0, 1000, false, ""),
                        job(3, 0, 1000, false, ""), job(4, 0, 1000, false, ""),
                        job(5, 0, 1000, false, ""), job(6, 0, 1000, false, "")]
            tryVerify(() => panel.visible)
            verify(panel.height <= root.height - 96)
        }

        function test_new_copy_is_above_old_errors() {
            const old = []
            for (let i = 1; i <= 6; ++i)
                old.push(job(i, 20, 1000, true, "Failed"))
            Ops.jobs = old.concat([job(77, 100, 1000, false, "")])
            tryVerify(() => panel.visible)
            const active = findChild(panel, "transferJob:77")
            verify(active !== null, "new active transfer must be inside the visible viewport")
            verify(active.y >= 0 && active.y < panel.height, "new copy is onscreen")
        }

        function test_failed_copy_is_not_reported_as_completed() {
            Ops.jobs = [job(52, 300, 1000, true, "Disk full")]
            tryVerify(() => panel.visible)
            const card = findChild(panel, "transferJob:52")
            verify(card !== null)
            compare(card.fraction, 0.3)
        }
    }
}
