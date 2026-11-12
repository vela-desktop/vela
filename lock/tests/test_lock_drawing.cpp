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
    QColor blurred = wallpaperBlurred)
{
    const QSize physical(int(logical.width() * scale120 / 120), int(logical.height() * scale120 / 120));
    QImage image(physical, QImage::Format_RGB32);
    QImage background(physical, QImage::Format_RGB32);
    background.fill(wallpaper);
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

    image = paint(look, clockScene(), { 800, 600 }, 120, QColor(235, 235, 235));
    check(anyPixel(image, timeBox, Qt::white), "behind it a light wallpaper: white");
    check(!anyPixel(image, timeBox, look.primary), "no primary there");
}

// A narrow output: the time and the date below it shrink to 90% of its
// width, in both looks. At 200 wide the date does not fit unshrunk, and
// neither size reaches its floor (24 and 12), under which nothing fits.
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
        check(fits(paint(look, scene, { 200, 600 })),
            look.expressive ? "the expressive date fits a very narrow output" : "the date fits a very narrow output");
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
    testClock();
    testNarrowOutput();
    testScale();
    if (failures == 0) {
        std::cout << "lock drawing: all tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
