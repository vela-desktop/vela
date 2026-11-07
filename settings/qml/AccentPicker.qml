// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// Colors' custom accent, like Windows 11's "View colors": a spectrum (hue
// across, saturation down) for the pointer and the arrows, a brightness
// slider, and the color's hex code to read or type (#RRGGBB, the # may be
// left out). The color before and the new one side by side; Done chooses it.
// open() starts from the accent in use.
Dialog {
    id: picker
    // The color edited, and its hue, saturation and brightness (0..1) for
    // the spectrum and the slider: kept apart, so a gray (no hue) or black
    // (no saturation) doesn't lose the others, and a typed code stays
    // exactly that code.
    property color color: "#000000"
    property real hue: 0
    property real saturation: 1
    property real brightness: 1
    property color before
    // What the hex field says, if it's a color.
    readonly property bool hexValid: code(hex.text) !== ""

    // `chosen` with "ok": the color to apply.
    signal accepted(color color)

    function start(from) {
        before = from
        setColor(from)
        hex.text = hexName()
        open()
        spectrum.forceActiveFocus()
    }
    // A whole color (the accent, a typed code): the spectrum and the
    // slider follow it.
    function setColor(c) {
        if (c.hsvHue >= 0) {
            hue = c.hsvHue
        }
        if (c.hsvValue > 0) {
            saturation = c.hsvSaturation
        }
        brightness = c.hsvValue
        color = c
    }
    // A point of the spectrum or the slider's brightness.
    function setHsv(h, s, v) {
        hue = Math.max(0, Math.min(1, h))
        saturation = Math.max(0, Math.min(1, s))
        brightness = Math.max(0, Math.min(1, v))
        color = Qt.hsva(hue, saturation, brightness, 1)
    }
    // A typed code as "#rrggbb", "" if it isn't one.
    function code(text) {
        const t = text.trim()
        return /^#?[0-9a-fA-F]{6}$/.test(t) ? (t.startsWith("#") ? t : "#" + t) : ""
    }
    function hexName() {
        return picker.color.toString().toUpperCase()
    }

    title: qsTr("Choose a custom accent color")
    dialogWidth: 400
    buttons: [
        { text: qsTr("Done"), accent: true, value: "ok", enabled: picker.hexValid },
        { text: qsTr("Cancel"), value: "cancel" }
    ]
    onChosen: value => { if (value === "ok") picker.accepted(picker.color) }
    // The fields change as the color does, except the one being typed in.
    onColorChanged: if (!hex.activeFocus) hex.text = hexName()

    Column {
        width: parent.width
        spacing: 16

        T.Control {
            id: spectrum
            objectName: "accentSpectrum"
            width: parent.width
            height: 180
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Hue and saturation")
            // The color where it is, then how to move it.
            Accessible.description: qsTr("Hue %1°, saturation %2%. Left and right change the hue, up and down the saturation")
                .arg(Math.round(picker.hue * 360)).arg(Math.round(picker.saturation * 100))

            function pick(x, y) {
                picker.setHsv(x / width, 1 - y / height, picker.brightness)
            }
            Keys.onLeftPressed: picker.setHsv(picker.hue - 4 / 360, picker.saturation, picker.brightness)
            Keys.onRightPressed: picker.setHsv(picker.hue + 4 / 360, picker.saturation, picker.brightness)
            Keys.onUpPressed: picker.setHsv(picker.hue, picker.saturation + 0.02, picker.brightness)
            Keys.onDownPressed: picker.setHsv(picker.hue, picker.saturation - 0.02, picker.brightness)

            background: Item {
                // The hues across, at full saturation and brightness...
                Rectangle {
                    id: hues
                    anchors.fill: parent
                    radius: Theme.radius
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0 / 6; color: "#ff0000" }
                        GradientStop { position: 1 / 6; color: "#ffff00" }
                        GradientStop { position: 2 / 6; color: "#00ff00" }
                        GradientStop { position: 3 / 6; color: "#00ffff" }
                        GradientStop { position: 4 / 6; color: "#0000ff" }
                        GradientStop { position: 5 / 6; color: "#ff00ff" }
                        GradientStop { position: 6 / 6; color: "#ff0000" }
                    }
                }
                // ...fading to white downwards (no saturation)...
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#00ffffff" }
                        GradientStop { position: 1; color: "#ffffffff" }
                    }
                }
                // ...and as dark as the brightness chosen.
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    color: "black"
                    opacity: 1 - picker.brightness
                }
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.controlStroke
                }
                // Where the color is.
                Rectangle {
                    objectName: "accentSpectrumMarker"
                    x: picker.hue * spectrum.width - width / 2
                    y: (1 - picker.saturation) * spectrum.height - height / 2
                    width: 16
                    height: 16
                    radius: 8
                    color: picker.color
                    border.width: 2
                    border.color: picker.brightness > 0.5 ? "black" : "white"
                }
                FocusRing {
                    visible: spectrum.visualFocus
                }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.CrossCursor
                onPressed: mouse => { spectrum.forceActiveFocus(); spectrum.pick(mouse.x, mouse.y) }
                onPositionChanged: mouse => { if (pressed) spectrum.pick(mouse.x, mouse.y) }
            }
        }

        Column {
            width: parent.width
            spacing: 4
            Text {
                text: qsTr("Brightness")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
            }
            Slider {
                id: brightnessSlider
                objectName: "accentBrightness"
                width: parent.width
                from: 0
                to: 100
                stepSize: 1
                value: Math.round(picker.brightness * 100)
                onMoved: picker.setHsv(picker.hue, picker.saturation, value / 100)
                Accessible.name: qsTr("Brightness")
            }
        }

        Row {
            width: parent.width
            spacing: 12
            // The color before and the new one.
            Row {
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    objectName: "accentBefore"
                    width: 40
                    height: 32
                    topLeftRadius: Theme.radius
                    bottomLeftRadius: Theme.radius
                    color: picker.before
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("Current color")
                }
                Rectangle {
                    objectName: "accentAfter"
                    width: 40
                    height: 32
                    topRightRadius: Theme.radius
                    bottomRightRadius: Theme.radius
                    color: picker.color
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("New color")
                }
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 92
                spacing: 4
                TextBox {
                    id: hex
                    objectName: "accentHex"
                    width: parent.width
                    maximumLength: 7
                    font.family: "monospace"
                    placeholderText: "#RRGGBB"
                    inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    Accessible.name: qsTr("Hex code")
                    // Typed, a whole code moves the spectrum and the slider.
                    onTextEdited: {
                        const typed = picker.code(text)
                        if (typed !== "")
                            picker.setColor(Qt.color(typed))
                    }
                    onActiveFocusChanged: if (!activeFocus && picker.hexValid) text = picker.hexName()
                    Keys.onReturnPressed: picker.accept()
                    Keys.onEnterPressed: picker.accept()
                }
                Text {
                    objectName: "accentHexError"
                    visible: !picker.hexValid
                    text: qsTr("A hex code has six digits, 0-9 and A-F, as #1E90FF")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    width: parent.width
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
