// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Files' places (src/places.h), as far as the navigation pane and the
// address bar read them: the tests' `Places` (views_test.cpp), with a home,
// a Recycle Bin, two pinned folders and two drives, one not mounted.
QtObject {
    property string home: "/home/ada"
    property string trash: "trash:"
    property string userName: "Ada"
    property var quickAccess: [
        { icon: "folder-documents", name: "Documents", path: "/home/ada/Documents" },
        { icon: "folder-pictures", name: "Pictures", path: "/home/ada/Pictures" }
    ]
    property var drives: [
        { icon: "drive-harddisk", name: "Local Disk", path: "/", volume: "", mounted: true, removable: false, device: "" },
        { icon: "drive-removable-media", name: "USB", path: "", volume: "usb-1", mounted: false, removable: true, device: "/dev/sdb1" }
    ]
    property var pinned: ["/home/ada/Documents", "/home/ada/Pictures"]

    function isPinned(path) { return pinned.indexOf(path) >= 0 }
    function pin(path) { pinned = pinned.concat([path]) }
    function unpin(path) { pinned = pinned.filter(p => p !== path) }
    function eject(device) {}
}
