// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The RHI glyph shaders, not Qt Quick's software painter: coverage must move
// smoothly through a physical pixel and must stay neutral over transparency.
// Read an explicit render target, avoiding swapchain age and window damage.

#include "renderingpolicy.h"
#include "controls.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTransform>
#include <QtQml/qqmlextensionplugin.h>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

namespace {

class Scene {
public:
    Scene()
        : window(&control)
    {
        vela::controls::install(engine, Appearance::Mode::Apps);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(TEXT_SCENE)));
        root.reset(qobject_cast<QQuickItem*>(component.create()));
        if (!root) {
            qWarning() << component.errors();
            return;
        }
        window.setColor(Qt::transparent);
        window.setGeometry(0, 0, int(root->width()), int(root->height()));
        window.contentItem()->setSize(root->size());
        root->setParentItem(window.contentItem());
    }

    ~Scene()
    {
        root.reset();
        control.invalidate();
        window.setRenderTarget({ });
    }

    bool initialize() { return root && control.initialize() && control.rhi(); }

    bool resize(qreal dpr)
    {
        window.setRenderTarget({ });
        target.reset();
        pass.reset();
        depth.reset();
        texture.reset();
        QRhi* rhi = control.rhi();
        const QSize size(qRound(root->width() * dpr), qRound(root->height() * dpr));
        texture.reset(rhi->newTexture(
            QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        if (!texture->create() || !depth->create()) {
            return false;
        }
        QRhiTextureRenderTargetDescription description { QRhiColorAttachment(texture.get()) };
        description.setDepthStencilBuffer(depth.get());
        target.reset(rhi->newTextureRenderTarget(description));
        pass.reset(target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        if (!target->create()) {
            return false;
        }
        auto quickTarget = QQuickRenderTarget::fromRhiRenderTarget(target.get());
        quickTarget.setDevicePixelRatio(dpr);
        window.setRenderTarget(quickTarget);
        return true;
    }

    QImage render()
    {
        QCoreApplication::processEvents();
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.render();
        QRhi* rhi = control.rhi();
        QRhiReadbackResult read;
        auto* update = rhi->nextResourceUpdateBatch();
        update->readBackTexture(texture.get(), &read);
        control.commandBuffer()->resourceUpdate(update);
        control.endFrame(); // offscreen submission waits for its readback
        if (read.data.isEmpty()) {
            return { };
        }
        QImage wrapper(reinterpret_cast<const uchar*>(read.data.constData()), read.pixelSize.width(),
            read.pixelSize.height(), QImage::Format_RGBA8888_Premultiplied);
        // Orientation does not affect coverage comparisons between frames.
        return wrapper.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    QQuickRenderControl control;
    QQuickWindow window;
    QQmlEngine engine;
    std::unique_ptr<QQuickItem> root;

private:
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiRenderBuffer> depth;
    std::unique_ptr<QRhiRenderPassDescriptor> pass;
    std::unique_ptr<QRhiTextureRenderTarget> target;
};

double coverage(const QImage& image)
{
    double total = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            total += qGreen(row[x]);
        }
    }
    return total;
}

double change(const QImage& previous, const QImage& next)
{
    if (previous.isNull() || next.size() != previous.size()) {
        return 1;
    }
    double difference = 0;
    for (int y = 0; y < next.height(); ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(previous.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(next.constScanLine(y));
        for (int x = 0; x < next.width(); ++x) {
            difference += std::abs(qGreen(a[x]) - qGreen(b[x]));
        }
    }
    return difference / std::max(coverage(previous), 1.0);
}

// Green-only metrics cannot tell an opaque black pixel from transparent black.
bool sameRgba(const QImage& first, const QImage& second)
{
    if (first.isNull() || second.size() != first.size()) {
        return false;
    }
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            const QRgb a = first.pixel(x, y), b = second.pixel(x, y);
            if (std::abs(qRed(a) - qRed(b)) > 2 || std::abs(qGreen(a) - qGreen(b)) > 2
                || std::abs(qBlue(a) - qBlue(b)) > 2 || std::abs(qAlpha(a) - qAlpha(b)) > 2) {
                return false;
            }
        }
    }
    return true;
}

// White text on transparency: premultiplied RGB must equal alpha coverage.
// The empty background must have zero alpha and glyph edges partial alpha.
bool transparentWhiteCoverage(const QImage& image)
{
    if (image.isNull() || image.pixel(0, 0) != qRgba(0, 0, 0, 0)) {
        return false;
    }
    bool foundGlyph = false, foundEdge = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            const int a = qAlpha(pixel);
            foundGlyph |= a > 128;
            foundEdge |= a > 0 && a < 255;
            if (std::abs(qRed(pixel) - a) > 2 || std::abs(qGreen(pixel) - a) > 2
                || std::abs(qBlue(pixel) - a) > 2) {
                return false;
            }
        }
    }
    return foundGlyph && foundEdge;
}

// Verify all premultiplied RGBA channels with a colored translucent surface.
// Allow a small 8-bit shader/rounding tolerance, not just green-channel parity.
bool translucentComposite(const QImage& transparent, const QImage& composite, const QColor& background)
{
    if (transparent.isNull() || composite.size() != transparent.size()) {
        return false;
    }
    const int alpha = background.alpha();
    constexpr int tolerance = 8;
    bool foundGlyph = false;
    for (int y = 0; y < transparent.height(); ++y) {
        for (int x = 0; x < transparent.width(); ++x) {
            const int coverage = qAlpha(transparent.pixel(x, y));
            const QRgb pixel = composite.pixel(x, y);
            foundGlyph |= coverage > 128;
            const auto expectedChannel = [&](int value) {
                return int(std::lround(value * alpha * (255 - coverage) / (255.0 * 255.0) + coverage));
            };
            const int expectedAlpha = int(std::lround(alpha + coverage * (255 - alpha) / 255.0));
            if (std::abs(qRed(pixel) - expectedChannel(background.red())) > tolerance
                || std::abs(qGreen(pixel) - expectedChannel(background.green())) > tolerance
                || std::abs(qBlue(pixel) - expectedChannel(background.blue())) > tolerance
                || std::abs(qAlpha(pixel) - expectedAlpha) > tolerance) {
                return false;
            }
        }
    }
    return foundGlyph;
}

bool neutralCoverage(const QImage& image)
{
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = row[x];
            if (std::max({ qRed(pixel), qGreen(pixel), qBlue(pixel) })
                        - std::min({ qRed(pixel), qGreen(pixel), qBlue(pixel) })
                    > 1
                || qGreen(pixel) > qAlpha(pixel) + 1) {
                return false;
            }
        }
    }
    return true;
}

double scrollSteps(Scene& scene, qreal dpr)
{
    scene.root->setProperty("scrollOffset", 0);
    QImage previous = scene.render();
    if (coverage(previous) < 255) {
        return std::numeric_limits<double>::infinity();
    }
    double largest = 0;
    for (int step = 1; step <= 64; ++step) {
        scene.root->setProperty("scrollOffset", step / (64.0 * dpr));
        const QImage next = scene.render();
        if (coverage(next) < 255) {
            return std::numeric_limits<double>::infinity();
        }
        largest = std::max(largest, change(previous, next));
        previous = next;
    }
    scene.root->setProperty("scrollOffset", 0);
    return largest;
}

double pressSteps(Scene& scene)
{
    scene.root->setProperty("pressScale", 1);
    QImage previous = scene.render();
    double largest = 0;
    for (int step = 1; step <= 256; ++step) {
        scene.root->setProperty("pressScale", 1 - 0.08 * step / 256);
        const QImage next = scene.render();
        largest = std::max(largest, change(previous, next));
        previous = next;
    }
    scene.root->setProperty("pressScale", 1);
    return largest;
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "rhi");
    // Exercise the production default, independently of the caller's desktop.
    qunsetenv("QSG_DISTANCEFIELD_ANTIALIASING");
    vela::rendering::configureApplication();
    // The policy belongs to the current renderer, not to child applications.
    if (qgetenv("QSG_DISTANCEFIELD_ANTIALIASING") != "gray"
        || vela::rendering::externalProcessEnvironment().contains(
            QStringLiteral("QSG_DISTANCEFIELD_ANTIALIASING"))) {
        std::fprintf(stderr, "FAIL: private text antialiasing leaked to child environment\n");
        return 1;
    }
    vela::rendering::configureApplication(); // Idempotent setup.
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(
        { QStringLiteral("capture-dir"), QStringLiteral("Save comparison frames"), QStringLiteral("directory") });
    parser.process(app);
    const QString captureDir = parser.value(QStringLiteral("capture-dir"));
    if (!captureDir.isEmpty() && !QDir().mkpath(captureDir)) {
        std::fprintf(stderr, "FAIL: cannot create capture directory\n");
        return 1;
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    Scene scene;
    if (!scene.root) {
        return 1;
    }
    if (!scene.initialize()) {
        std::fprintf(stderr, "SKIP: cannot initialize the OpenGL RHI scene graph\n");
        return 77;
    }
    auto* label = scene.root->findChild<QQuickItem*>(QStringLiteral("label"));
    if (!label) {
        return 1;
    }
    const QFont font = label->property("font").value<QFont>();
    std::printf("OpenGL RHI device: %s; font: %s\n", scene.control.rhi()->driverInfo().deviceName.constData(),
        qPrintable(font.family()));
    for (const qreal dpr : { 1.0, 1.25, 1.5, 1.75, 2.0 }) {
        if (!scene.resize(dpr)) {
            std::fprintf(stderr, "FAIL: cannot create the render target at %g\n", dpr);
            return 1;
        }
        for (const int pixelSize : { 12, 14, 18 }) {
            QElapsedTimer measurement;
            measurement.start();
            scene.root->setProperty("pixelSize", pixelSize);
            const auto capture = [&](const QImage& image, const QString& name) {
                if (captureDir.isEmpty()) {
                    return true;
                }
                const QImage oriented = scene.control.rhi()->isYUpInFramebuffer()
                    ? image.transformed(QTransform::fromScale(1, -1))
                    : image;
                return oriented.save(
                    QDir(captureDir).filePath(QStringLiteral("%1-%2-%3.png").arg(dpr).arg(pixelSize).arg(name)));
            };
            const QImage rest = scene.render();
            if (!capture(rest, QStringLiteral("rest"))) {
                return 1;
            }
            if (coverage(rest) < 255 || !neutralCoverage(rest)) {
                std::fprintf(stderr, "FAIL: missing glyphs or colored fringes at %g, %dpx\n", dpr, pixelSize);
                return 1;
            }
            const double scroll = scrollSteps(scene, dpr);
            const double press = pressSteps(scene);
            const QImage returned = scene.render();
            scene.root->setProperty("color", QColor(Qt::transparent));
            const QImage transparent = scene.render();
            if (!capture(transparent, QStringLiteral("transparent"))) {
                return 1;
            }
            const QColor coloredSurface(40, 100, 180, 128);
            scene.root->setProperty("color", coloredSurface);
            const QImage tinted = scene.render();
            if (!capture(tinted, QStringLiteral("tinted"))) {
                return 1;
            }
            scene.root->setProperty("color", QColor(Qt::black));
            // Continuity thresholds are separate from premultiplied RGBA checks.
            if (scroll > 0.06 || press > 0.08 || !sameRgba(rest, returned)
                || !neutralCoverage(transparent) || !transparentWhiteCoverage(transparent)
                || !translucentComposite(transparent, tinted, coloredSurface)
                || change(rest, transparent) > 0.001) {
                std::fprintf(stderr,
                    "FAIL: dpr=%g font=%d scroll=%g press=%g stable=%d white-alpha=%d tinted=%d\n",
                    dpr, pixelSize, scroll, press, int(sameRgba(rest, returned)),
                    int(transparentWhiteCoverage(transparent)),
                    int(translucentComposite(transparent, tinted, coloredSurface)));
                return 1;
            }
            scene.root->setProperty("pressScale", 0.92);
            if (!capture(scene.render(), QStringLiteral("pressed"))) {
                return 1;
            }
            scene.root->setProperty("pressScale", 1);
            scene.root->setProperty("scrollOffset", 0.4 / dpr);
            if (!capture(scene.render(), QStringLiteral("scrolled"))) {
                return 1;
            }
            scene.root->setProperty("scrollOffset", 0);
            // A native bitmap is the negative control: its whole line jumps at a
            // rounding boundary, rather than changing coverage through the pixel.
            label->setProperty("renderType", 1); // Text.NativeRendering
            scene.render();
            const double nativeScroll = scrollSteps(scene, dpr);
            scene.root->setProperty("pressScale", 0.92);
            if (!capture(scene.render(), QStringLiteral("native-pressed"))) {
                return 1;
            }
            scene.root->setProperty("pressScale", 1);
            label->setProperty("renderType", 0); // Text.QtRendering
            scene.render();
            if (!std::isfinite(nativeScroll) || nativeScroll <= 0.06) {
                std::fprintf(stderr, "FAIL: insensitive native control at %g (%g)\n", dpr, nativeScroll);
                return 1;
            }
            // Diagnostics only: elapsed time is not a CI performance threshold.
            std::printf("PASS: dpr=%g font=%d scroll=%g press=%g native-scroll=%g elapsed-ms=%lld\n",
                dpr, pixelSize, scroll, press, nativeScroll, static_cast<long long>(measurement.elapsed()));
        }
    }
    return 0;
}
