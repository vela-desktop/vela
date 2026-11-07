// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls
import Vela.Updates

// System > Updates (docs/updates.md §9.1), on vela-update-daemon: the state
// and its button, what's new, and the options. Seeing an update here counts
// as being told about it (§6.4): the shell won't announce it again.
Page {
    readonly property string announce: Updates.announce
    onAnnounceChanged: if (announce !== "") Updates.announced(announce)
    Component.onCompleted: if (announce !== "") Updates.announced(announce)

    UpdateStatusCard {}
    WhatsNew {}
    UpdateOptions {
        dialogParent: root.contentItem
    }
}
