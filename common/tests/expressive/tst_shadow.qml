// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls

// Shadow under the expressive theme: besides the four strips around the
// target, its corners inside the target's box, so M3's large round corners
// leave no lighter square in the shadow.
Item {
    id: root
    width: 600
    height: 500

    Item { id: panel; x: 100; y: 100; width: 200; height: 150 }
    Shadow { id: shadow; target: panel; radius: Theme.shapeExtraLarge }
    Item { id: small; x: 400; y: 100; width: 30; height: 20 }
    Shadow { id: smallShadow; target: small; radius: Theme.shapeExtraLarge }

    TestCase {
        name: "ShadowExpressive"
        when: windowShown

        function init() {
            if (!Theme.expressive)
                skip("with the expressive theme (the *-expressive ctest entries)")
        }

        function corners(item) {
            const list = []
            for (let i = 4; i < item.children.length; ++i) {
                if (item.children[i].width !== undefined && item.children[i].index !== undefined)
                    list.push(item.children[i])
            }
            return list
        }

        function test_the_corners_are_shadowed() {
            const pieces = corners(shadow)
            compare(pieces.length, 4)
            const r = Theme.shapeExtraLarge
            const at = pieces.map(p => [p.x - shadow.reach, p.y - shadow.reach, p.width, p.height])
            compare(at, [[0, 0, r, r], [200 - r, 0, r, r], [0, 150 - r, r, r], [200 - r, 150 - r, r, r]])
        }

        // A target smaller than its corners: no piece crosses its middle.
        function test_small_targets() {
            const pieces = corners(smallShadow)
            compare(pieces.length, 4)
            for (const piece of pieces)
                compare(piece.width, 10)
        }
    }
}
