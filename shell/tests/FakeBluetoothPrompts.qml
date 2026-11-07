// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// BluetoothPrompts (bluetoothprompts.h) for the dialog's tests: they set the
// question the dialog shows, and it records the answers ("accept 123456").
QtObject {
    property var dialog: ({})
    property var answers: []

    function answer(response, value) { answers = answers.concat([value ? response + " " + value : response]) }
    function reset() {
        dialog = {}
        answers = []
    }
}
