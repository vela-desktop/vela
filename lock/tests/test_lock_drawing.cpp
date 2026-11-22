// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The lock screen's drawing (lock/drawing.h), painted offscreen on plain
// wallpapers and read back pixel by pixel: the clock and the login screen in
// the default look and in the expressive one (look.h), at 100% and 200%,
// and on an output too narrow for the time.

#include "../drawing.h"

#include <QGuiApplication>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const std::string& description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

using vela::accent::Source;
using vela::lock::Look;
using vela::lock::Mode;
using vela::lock::Scene;

const QColor wallpaper(20, 24, 40);
const QColor wallpaperBlurred(40, 40, 40);

bool near(QColor a, QColor b, int tolerance = 2)
{
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance
        && std::abs(a.blue() - b.blue()) <= tolerance;
}

// `color` under black at `alpha`, as the lock dims the wallpaper.
QColor dimmed(QColor color, int alpha)
{
    const double keep = 1.0 - alpha / 255.0;
    return QColor(int(std::lround(color.red() * keep)), int(std::lround(color.green() * keep)),
        int(std::lround(color.blue() * keep)));
}

// `color` at `alpha` over `under`.
QColor over(QColor color, int alpha, QColor under)
{
    const double a = alpha / 255.0;
    return QColor(int(std::lround(color.red() * a + under.red() * (1 - a))),
        int(std::lround(color.green() * a + under.green() * (1 - a))),
        int(std::lround(color.blue() * a + under.blue() * (1 - a))));
}

QImage paint(const Look& look, const Scene& scene, QSize logical = { 800, 600 }, uint32_t scale120 = 120,
    QColor blurred = wallpaperBlurred, QColor sharp = wallpaper, QRect detail = {})
{
    const QSize physical(int(logical.width() * scale120 / 120), int(logical.height() * scale120 / 120));
    QImage image(physical, QImage::Format_RGB32);
    QImage background(physical, QImage::Format_RGB32);
    background.fill(sharp);
    // A white detail of the sharp wallpaper, gone in the blurred one.
    for (int y = detail.top(); y <= detail.bottom(); ++y) {
        for (int x = detail.left(); x <= detail.right(); ++x) {
            background.setPixelColor(x, y, Qt::white);
        }
    }
    QImage behind(physical, QImage::Format_RGB32);
    behind.fill(blurred);
    vela::lock::paint(image, background, behind, logical, scale120, look, scene);
    return image;
}

QColor at(const QImage& image, double x, double y, double scale = 1)
{
    return image.pixelColor(int(x * scale), int(y * scale));
}

bool anyPixel(const QImage& image, QRect box, QColor color, int tolerance = 0)
{
    box = box.intersected(image.rect());
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            if (near(image.pixelColor(x, y), color, tolerance)) {
                return true;
            }
        }
    }
    return false;
}

Look defaultLook()
{
    return vela::lock::lookFrom({}, false, Source::Theme, QColor(0, 120, 60), QColor());
}

Look expressiveLook()
{
    return vela::lock::lookFrom({ { QStringLiteral("controls-variant"), QStringLiteral("expressive") } }, false,
        Source::Theme, QColor(0, 120, 60), QColor());
}

Scene loginScene(int characters = 0)
{
    Scene scene;
    scene.mode = Mode::Login;
    scene.now = QDateTime(QDate(2026, 11, 12), QTime(12, 45));
    scene.timeFormat = QStringLiteral("HH:mm");
    scene.displayName = QStringLiteral("Ada");
    scene.passwordEmpty = characters == 0;
    scene.passwordChars = characters;
    return scene;
}

Scene clockScene()
{
    Scene scene = loginScene();
    scene.mode = Mode::Clock;
    return scene;
}

// On an 800x600 output: the avatar's circle around (400, 255), 75 across;
// the name at 350; the field from y 410, 300 wide from x 250.
void testDefaultLogin()
{
    const Look look = defaultLook();
    const QImage image = paint(look, loginScene());
    const QColor behind = dimmed(wallpaperBlurred, 110);
    check(near(at(image, 5, 5), behind), "the login screen is on the blurred wallpaper, dimmed");
    check(near(at(image, 400, 195), look.accent), "the avatar in the accent");
    check(near(at(image, 400, 449), look.accent), "the field's accent line along its bottom");
    check(near(at(image, 530, 425), over(Qt::white, 36, behind), 3), "the field's translucent white");
}

void testExpressiveLogin()
{
    const Look look = expressiveLook();
    const QImage image = paint(look, loginScene());
    const QColor behind = dimmed(wallpaperBlurred, 110);
    check(near(at(image, 400, 195), look.primaryContainer), "the avatar in primary container");
    check(near(at(image, 530, 434), look.surfaceContainerHighest), "the field on surface container highest");
    check(near(at(image, 400, 456), look.surfaceContainerHighest), "48 tall, with no accent line");
    check(!anyPixel(image, QRect(250, 450, 300, 9), look.accent), "nowhere the default look's line");
    check(near(at(image, 251, 411), behind), "a full pill: its corners show the wallpaper");
    check(near(at(image, 254, 434), look.surfaceContainerHighest), "the pill reaches its ends at the middle");
}

// One dot per character, in the look's color, from the text's start.
void testPasswordDots()
{
    const Look plain = defaultLook();
    QImage image = paint(plain, loginScene(3));
    for (int i = 0; i < 3; ++i) {
        check(near(at(image, 264 + 3.5 + i * 12, 430), Qt::white), "a white dot per character");
    }
    check(!near(at(image, 264 + 3.5 + 3 * 12, 430), Qt::white), "three dots, not four");

    const Look look = expressiveLook();
    image = paint(look, loginScene(3));
    for (int i = 0; i < 3; ++i) {
        check(near(at(image, 274 + 3.5 + i * 12, 434), look.onSurface), "a dot in on surface per character");
    }
    check(near(at(image, 274 + 3.5 + 3 * 12, 434), look.surfaceContainerHighest), "three dots, not four");
    image = paint(look, loginScene(40));
    check(near(at(image, 274 + 3.5 + 31 * 12, 434), look.onSurface), "up to 32 dots");
    check(!near(at(image, 274 + 3.5 + 32 * 12, 434), look.onSurface), "never more than 32");
    check(vela::lock::passwordCharacters("p\xc3\xa0ss") == 4, "a multibyte character is one dot");
}

void testMessage()
{
    Scene scene = loginScene();
    const QRect below(0, 466, 800, 30);
    const QColor behind = dimmed(wallpaperBlurred, 110);
    QImage image = paint(defaultLook(), scene);
    check(!anyPixel(image, below, Qt::white, 40), "no message, nothing below the field");
    scene.message = QStringLiteral("The password is incorrect. Try again.");
    image = paint(defaultLook(), scene);
    check(anyPixel(image, below, over(Qt::white, 220, behind), 30), "the message below the field");
}

void testCapsLock()
{
    const QColor caution(0xfc, 0xe1, 0x00);
    Scene scene = loginScene();
    const QRect below(0, 466, 800, 30);
    // The message's own line, and the ones under it: the warning starts a
    // line of the font plus 6 further down, wherever the font puts its
    // glyphs.
    const QRect messageLine(0, 466, 800, 16);
    const QRect further(0, 486, 800, 40);
    check(!anyPixel(paint(defaultLook(), scene), below, caution, 40), "Caps Lock off: no warning");
    scene.capsLock = true;
    check(anyPixel(paint(defaultLook(), scene), below, caution, 40), "Caps Lock on: the warning under the field");
    check(anyPixel(paint(expressiveLook(), scene), below, caution, 40), "in the expressive look too");
    // With a message, below it.
    scene.message = QStringLiteral("The password is incorrect. Try again.");
    QImage image = paint(defaultLook(), scene);
    check(!anyPixel(image, messageLine, caution, 40), "the message keeps its place");
    check(anyPixel(image, further, caution, 40), "the warning under the message");
    // Not while signing in, nor on the clock.
    scene.message.clear();
    scene.mode = Mode::Checking;
    check(!anyPixel(paint(defaultLook(), scene), below, caution, 40), "not while signing in");
    Scene clock = clockScene();
    clock.capsLock = true;
    check(!anyPixel(paint(defaultLook(), clock), QRect(0, 0, 800, 600), caution, 40), "not on the clock");
}

// The Caps Lock display (docs/keyboard.md §8.3): a pill at the bottom in
// the middle, in every mode, the arrow filled when Caps Lock is on.
void testCapsDisplay()
{
    const QRect pillBox(300, 500, 200, 52); // 48 from the bottom, 52 tall
    // A light wallpaper, for the dark pill to show.
    const QColor light(200, 160, 120);
    // With a password typed too: the dots leave a white brush behind.
    for (Scene scene : { loginScene(), loginScene(3), clockScene() }) {
        const QImage without = paint(defaultLook(), scene, { 800, 600 }, 120, light, light);
        scene.capsDisplay = 1;
        scene.capsDisplayOn = true;
        const QImage with = paint(defaultLook(), scene, { 800, 600 }, 120, light, light);
        check(without.copy(0, 0, 800, 498) == with.copy(0, 0, 800, 498), "nothing else changes");
        check(!anyPixel(without, pillBox, Qt::white, 40), "no display, nothing at the bottom");
        check(anyPixel(with, pillBox, Qt::white, 40), "the display at the bottom, in white");
        check(near(at(with, 400, 503), over(QColor(32, 32, 32), 230, at(without, 400, 503)), 3), "on a dark pill");
        check(near(at(with, 400, 549), over(QColor(32, 32, 32), 230, at(without, 400, 549)), 3), "52 tall");
        check(near(at(with, 400, 554), at(without, 400, 554)), "48 from the bottom");
        // At least 180 wide and in the middle; a wider font widens it.
        int left = -1;
        int right = -1;
        for (int x = 0; x < 800; ++x) {
            if (!near(at(with, x, 526), at(without, x, 526), 20)) {
                left = left < 0 ? x : left;
                right = x;
            }
        }
        check(left >= 0 && right - left + 1 >= 179 && std::abs((left + right + 1) - 800) <= 2,
            "at least 180 wide, in the middle");
    }
    // Expressive: the field's surface container highest, its ink.
    Scene scene = loginScene();
    scene.capsDisplay = 1;
    const Look look = expressiveLook();
    QImage image = paint(look, scene);
    check(near(at(image, 400, 503), look.surfaceContainerHighest, 3), "expressive: surface container highest");
    check(anyPixel(image, pillBox, look.onSurface, 30), "expressive: the text in on surface");
    // After a failed sign-in with Caps Lock on, the message and the warning
    // reach the bottom: the pill goes below the warning, never over it.
    for (const Look& each : { defaultLook(), expressiveLook() }) {
        Scene warned = loginScene();
        warned.message = QStringLiteral("The password is incorrect. Try again.");
        warned.capsLock = true;
        const QImage plain = paint(each, warned);
        int warningBottom = -1;
        for (int y = 0; y < 600; ++y) {
            if (anyPixel(plain, QRect(0, y, 800, 1), QColor(0xfc, 0xe1, 0x00), 40)) {
                warningBottom = y;
            }
        }
        check(warningBottom > 0, "the warning is drawn");
        warned.capsDisplay = 1;
        const QImage both = paint(each, warned);
        check(both.copy(0, 0, 800, warningBottom + 1) == plain.copy(0, 0, 800, warningBottom + 1),
            "the pill leaves the warning alone");
        check(!(both == plain), "and still shows");
    }
    // Half faded: halfway to what is behind.
    const QColor behind = at(paint(look, loginScene()), 400, 503);
    scene.capsDisplay = 0.5;
    image = paint(look, scene);
    check(near(at(image, 400, 503), over(look.surfaceContainerHighest, 128, behind), 4), "half faded");

    // The arrow: filled with a bar when on, an outline when off.
    QImage arrow(48, 48, QImage::Format_RGB32);
    for (const bool on : { true, false }) {
        arrow.fill(Qt::black);
        QPainter p(&arrow);
        p.setRenderHint(QPainter::Antialiasing);
        vela::lock::paintCapsArrow(p, QRectF(0, 0, 48, 48), Qt::white, on);
        p.end();
        check(near(at(arrow, 24, 28), Qt::white, 10) == on, on ? "on: a filled arrow" : "off: an outline");
        if (on) {
            check(near(at(arrow, 24, 35), Qt::black, 10) && near(at(arrow, 24, 40), Qt::white, 10),
                "on: a bar under it, apart");
        } else {
            check(near(at(arrow, 24, 41), Qt::white, 10) && near(at(arrow, 24, 46), Qt::black, 10),
                "off: the outline closed at the bottom, no bar");
        }
        check(near(at(arrow, 24, 0), Qt::black, 10) && anyPixel(arrow, QRect(20, 2, 9, 6), Qt::white, 60),
            "the arrow's tip at the top");
    }

    // Whole for the hold, then down to nothing; at once without a fade.
    check(vela::lock::capsDisplayOpacity(0, 1500, 200) == 1, "whole when shown");
    check(vela::lock::capsDisplayOpacity(1499, 1500, 200) == 1, "whole until the hold ends");
    check(qFuzzyCompare(vela::lock::capsDisplayOpacity(1600, 1500, 200), 0.5), "half way through the fade");
    check(vela::lock::capsDisplayOpacity(1700, 1500, 200) == 0, "gone after the fade");
    check(vela::lock::capsDisplayOpacity(1500, 1500, 0) == 0, "no fade: gone at once");
}

// The clock: the default look's white, the expressive primary on a dark
// wallpaper and white where nothing lighter reads.
void testClock()
{
    const QRect timeBox(0, 72, 800, 200);
    const QColor behind = dimmed(wallpaper, 60);
    const Look plain = defaultLook();
    QImage image = paint(plain, clockScene());
    check(near(at(image, 5, 5), behind), "the clock is on the wallpaper, dimmed less");
    check(anyPixel(image, timeBox, over(Qt::white, 240, behind), 1), "the default time in white");

    const Look look = expressiveLook();
    image = paint(look, clockScene());
    const QColor expected = vela::lock::clockColor(look.primary, dimmed(wallpaperBlurred, 60));
    check(expected == look.primary, "on a dark wallpaper the clock is primary");
    check(anyPixel(image, timeBox, look.primary), "the expressive time in primary");
    check(!anyPixel(image, timeBox, over(Qt::white, 240, behind), 1), "not in the default white");

    image = paint(look, clockScene(), { 800, 600 }, 120, QColor(235, 235, 235), QColor(235, 235, 235));
    check(anyPixel(image, timeBox, Qt::white), "behind it a light wallpaper: white");
    check(!anyPixel(image, timeBox, look.primary), "no primary there");

    // The clock is over the sharp wallpaper: its color is read against that
    // one, a small bright detail included, not against the blurred one.
    image = paint(look, clockScene(), { 800, 600 }, 120, wallpaperBlurred, wallpaper, QRect(399, 100, 3, 3));
    check(!anyPixel(image, timeBox, look.primary), "a bright detail behind the time: no primary");
    image = paint(look, clockScene(), { 800, 600 }, 120, QColor(235, 235, 235));
    check(anyPixel(image, timeBox, look.primary), "a light blurred wallpaper, not shown behind the clock, changes nothing");
}

// A narrow output: the time and the date below it shrink to 90% of its
// width, in both looks.
void testNarrowOutput()
{
    for (const Look& look : { defaultLook(), expressiveLook() }) {
        Scene scene = clockScene();
        scene.timeFormat = QStringLiteral("hh:mm AP");
        QImage image = paint(look, scene, { 300, 600 });
        const QColor behind = dimmed(wallpaper, 60);
        // The time's box and the date's: 72 to 266 by default, to 316 expressive.
        const auto fits = [&behind](const QImage& picture) {
            for (int y = 72; y < 320; ++y) {
                for (const int x : { 0, 1, 2 }) {
                    if (!near(picture.pixelColor(x, y), behind, 1)
                            || !near(picture.pixelColor(picture.width() - 1 - x, y), behind, 1)) {
                        return false;
                    }
                }
            }
            return true;
        };
        const bool inside = fits(image);
        // 250 wide: the date does not fit unshrunk in an ordinary font, and
        // still fits at its smallest size (12) in the boxes Qt draws when
        // no font is installed, one per letter and as wide as the size.
        check(fits(paint(look, scene, { 250, 600 })),
            look.expressive ? "the expressive date fits a narrower output" : "the date fits a narrower output");
        check(inside, look.expressive ? "the expressive time fits a narrow output" : "the time fits a narrow output");
        check(anyPixel(image, QRect(15, 72, 270, 200), behind, 0)
                && image.pixelColor(150, 5) == image.pixelColor(5, 5),
            "the rest of the narrow output is the wallpaper");
    }
}

// At 200% everything is twice as large, at the same logical places.
void testScale()
{
    const Look look = expressiveLook();
    const QImage image = paint(look, loginScene(), { 800, 600 }, 240);
    check(image.size() == QSize(1600, 1200), "the buffer in physical pixels");
    check(near(at(image, 400, 195, 2), look.primaryContainer), "the avatar at its logical place");
    check(near(at(image, 530, 434, 2), look.surfaceContainerHighest), "the field at its logical place");
    check(near(at(image, 251, 411, 2), dimmed(wallpaperBlurred, 110)), "the pill's corner at its logical place");
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    testDefaultLogin();
    testExpressiveLogin();
    testPasswordDots();
    testMessage();
    testCapsLock();
    testCapsDisplay();
    testClock();
    testNarrowOutput();
    testScale();
    if (failures == 0) {
        std::cout << "lock drawing: all tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
