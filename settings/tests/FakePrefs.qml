// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// A stand-in for Settings' Prefs (settings/src/preferences.h) under Qt Quick
// Test (settings_test.cpp makes it the context's Prefs): what
// Personalization's pieces read and call, with made-up themes and styles,
// and the changes they asked for in `calls`. A test's init() calls reset().
QtObject {
    // Colors.
    property color accent: "#0078d4"
    property string appTheme: "dark"
    property string shellTheme: "dark"
    readonly property bool light: appTheme === "light"
    // A picture in the tree, so that the previews have a wallpaper to load.
    property string wallpaper: Qt.resolvedUrl("../../themes/screenshots/themes/vela.png").toString().replace("file://", "")

    // Personalization.
    property string visualTheme: "vela"
    property var surfaceStyles: ({})
    property int themesRevision: 0
    property string taskbarAlignment: "center"
    // What styleGallery() lists: the themes ("") and each surface's styles.
    property var themes: []
    property var styles: ({})
    // Each theme's tokens, as resolveTheme() gives them (its chosen styles
    // merged): a test sets them, then bumps themesRevision as a file would.
    property var tokens: ({})
    // resolveTheme()'s layoutFromStyle: the taskbar style sets the layout.
    property bool layoutFromStyle: false
    // "From the theme"'s pictures (themeSurfacePictures()), by "<theme> <surface>":
    // { images, from }.
    property var surfacePictures: ({})
    // The changes asked for, in order: [call, arguments...].
    property var calls: []

    function reset() {
        accent = "#0078d4"
        appTheme = "dark"
        shellTheme = "dark"
        visualTheme = "vela"
        surfaceStyles = { taskbar: "", startmenu: "", notifications: "" }
        taskbarAlignment = "center"
        themes = [
            { name: "vela", text: "Vela Official", author: "Brando Giuffrida", images: [] },
            { name: "glass", text: "Glass", author: "Lowell137", images: [] },
            { name: "mine", text: "Mine", author: "", images: [] }
        ]
        styles = {
            taskbar: [
                { name: "pill", text: "Pill", author: "Lowell137", images: [] },
                { name: "dock", text: "Dock", author: "Lowell137", images: [] }
            ],
            startmenu: [{ name: "square", text: "Square", author: "Lowell137", images: [] }],
            notifications: [{ name: "round", text: "Round", author: "Lowell137", images: [] }]
        }
        tokens = {}
        layoutFromStyle = false
        surfacePictures = {}
        calls = []
        ++themesRevision
    }

    function record(call) {
        calls = calls.concat([call])
    }

    function visualThemes() {
        return themes.map(t => ({ name: t.name, text: t.text, author: t.author }))
    }
    function styleGallery(surface) {
        return surface === "" ? themes : (styles[surface] || [])
    }
    function themeSurfacePictures(theme, surface) {
        return surfacePictures[theme + " " + surface] || { images: [], from: "" }
    }
    function resolveTheme(name) {
        return { installed: themes.some(t => t.name === name), tokens: tokens[name] || {}, problems: [],
            layoutFromStyle: layoutFromStyle }
    }
    function setSurfaceStyle(surface, style) {
        const chosen = Object.assign({}, surfaceStyles)
        chosen[surface] = style
        surfaceStyles = chosen
        record(["setSurfaceStyle", surface, style])
    }
    function resetThemes() {
        visualTheme = "vela"
        surfaceStyles = { taskbar: "", startmenu: "", notifications: "" }
        record(["resetThemes"])
    }
    function setThemeAndStyles(theme, chosen) {
        visualTheme = theme
        surfaceStyles = Object.assign({}, chosen)
        record(["setThemeAndStyles", theme, Object.assign({}, chosen)])
    }

    Component.onCompleted: reset()
}
