// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls

// Corners inside corners (docs/visual-coherence.md §6.12): Theme.nested's
// rule, and the pieces that apply it for whoever uses them: a Shadow takes
// its target's corners, a Button and a ListItem in a rounder shape's corner
// follow it, a RoundClip holds a picture.
Item {
    id: root
    width: 600
    height: 500

    Rectangle { id: round; x: 60; y: 60; width: 200; height: 120; radius: 28 }
    Item { id: plain; x: 300; y: 60; width: 200; height: 120 }
    Shadow { id: follows; target: round }
    Shadow { id: fallback; target: plain }
    Shadow { id: asked; target: round; radius: 5 }

    // Fluent, whatever the theme of the run: an expressive button is a pill,
    // a shape of its own.
    Button { id: alone; styleVariant: "default"; text: "Alone" }
    Button { id: cornered; y: 40; styleVariant: "default"; text: "Cornered"; outerRadius: 28; outerGap: 8 }
    Button { id: far; y: 80; styleVariant: "default"; text: "Far"; outerRadius: 8; outerGap: 16 }
    Button { id: pill; y: 120; styleVariant: "expressive"; text: "Pill"; outerRadius: 28; outerGap: 8 }
    ListItem { id: item; y: 200; width: 200; height: 36; styleVariant: "default"; text: "Item"; outerRadius: 12; outerGap: 4 }
    ListItem { id: itemAlone; y: 240; width: 200; height: 36; styleVariant: "default"; text: "Item" }

    RoundClip {
        id: clip
        x: 300
        y: 300
        width: 120
        height: 80
        radius: Theme.nested(12, 6, 0)
        Rectangle { id: held; anchors.fill: parent; color: "red" }
    }
    RoundClip { id: square; x: 300; y: 400; width: 60; height: 40; Rectangle { anchors.fill: parent } }

    TestCase {
        name: "Corners"
        when: windowShown

        function test_nested_rule_data() {
            return [
                { tag: "in the corner", outer: 28, gap: 12, own: 4, radius: 16 },
                { tag: "a rounder own radius still follows", outer: 20, gap: 16, own: 16, radius: 4 },
                { tag: "at the edge", outer: 8, gap: 0, own: 4, radius: 8 },
                { tag: "as far as the radius: its own", outer: 8, gap: 8, own: 4, radius: 4 },
                { tag: "farther: its own", outer: 8, gap: 16, own: 4, radius: 4 },
                { tag: "a square shape: its own", outer: 0, gap: 4, own: 6, radius: 6 },
                { tag: "square in square", outer: 0, gap: 0, own: 0, radius: 0 },
            ]
        }

        // Both arcs share a center: the outer radius less the gap; where
        // the outer corner is out of reach, the shape's own.
        function test_nested_rule(data) {
            compare(Theme.nested(data.outer, data.gap, data.own), data.radius)
        }

        function test_shadow_takes_its_targets_corners() {
            compare(follows.radius, 28)
            round.radius = 12
            compare(follows.radius, 12)
            round.radius = 28
            // A target without corners: the overlays' radius; and what is
            // asked wins.
            compare(fallback.radius, Theme.radiusOverlay)
            compare(asked.radius, 5)
        }

        function test_button_in_a_corner() {
            // (the radius is animated: an expressive button's morphs)
            tryCompare(alone.background, "radius", Math.min(Theme.radius, alone.height / 2), 5000)
            tryCompare(cornered.background, "radius", Math.min(20, cornered.height / 2), 5000)
            tryCompare(far.background, "radius", Math.min(Theme.radius, far.height / 2), 5000)
            // Expressive: a pill wherever it is.
            tryCompare(pill.background, "radius", pill.height / 2, 5000)
        }

        function test_list_item_in_a_menu() {
            compare(item.background.radius, 8)
            compare(itemAlone.background.radius, Theme.radius)
        }

        function test_round_clip() {
            compare(clip.radius, 6)
            compare(held.parent.width, 120)
            compare(held.parent.height, 80)
            verify(held.parent.clip)
            const mask = findChild(clip, "roundClipMask")
            verify(mask)
            compare(mask.radius, 6)
            // Square, or without shaders (these tests): a plain clip, no layer.
            verify(!square.masked)
            compare(clip.masked, GraphicsInfo.api !== GraphicsInfo.Software && GraphicsInfo.api !== GraphicsInfo.Unknown
                    && GraphicsInfo.api !== GraphicsInfo.Null)
        }
    }
}
