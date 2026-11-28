// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A file's icon: the thumbnail if there is one (images, videos, PDFs),
// otherwise the type's icon. They load without stalling the view. Shortcuts
// get the arrow at the bottom left, like in Windows.
Item {
    id: icon
    property string path
    property string iconName
    property bool thumbnail: false
    property var modified
    property int size: 16
    property bool cut: false
    property bool link: false

    width: size
    height: size
    opacity: cut ? 0.5 : 1

    Image {
        id: themed
        anchors.fill: parent
        visible: !thumb.visible
        source: Theme.fileIcons + encodeURIComponent(icon.iconName)
        sourceSize: Qt.size(icon.size, icon.size)
        asynchronous: icon.size > 32
        smooth: true
    }
    Image {
        id: thumb
        anchors.fill: parent
        visible: status === Image.Ready
        // Only where it's clearly visible: medium and large icons.
        source: icon.thumbnail && icon.size >= 48
            ? "image://filethumb/" + encodeURIComponent(icon.path) + "/" + (icon.modified ? icon.modified.getTime() : 0) : ""
        sourceSize: Qt.size(icon.size, icon.size)
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: false
    }
    // The shortcut arrow.
    Image {
        visible: icon.link
        anchors { left: parent.left; bottom: parent.bottom }
        width: Math.max(10, Math.round(icon.size * 0.4))
        height: width
        source: Theme.icons + "emblem-symbolic-link"
        sourceSize: Qt.size(width, height)
    }
}
