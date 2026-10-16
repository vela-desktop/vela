// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// An icon of the theme (Theme.icons), drawn at its size in the output's own
// pixels: `name` is the theme's name, `size` its side in units. `onAccent`:
// the variant for an accent background. No name, no image.
Image {
    property string name
    property int size: 16
    property bool onAccent: false
    width: size
    height: size
    source: name === "" ? "" : (onAccent ? Theme.iconsOnAccent : Theme.icons) + encodeURIComponent(name)
    sourceSize: Qt.size(width, height)
}
