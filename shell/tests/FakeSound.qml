// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The sound keys (shell/src/soundkeys.h), as far as the volume indicator
// is concerned: the tests' `Sound` (shell_test.cpp).
QtObject {
    signal osdRequested(string target)
}
