// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The RHI glyph shaders, not Qt Quick's software painter: coverage must move
// smoothly through a physical pixel and must stay neutral over transparency.
// Read an explicit render target, avoiding swapchain age and window damage.

#include "renderingpolicy.h"
#include "controls.h"

#include <QCommandLineParser>
#include <QDir>
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
            scene.root->setProperty("color", QColor(Qt::black));
            // A 1/64 pixel translation should change only edge coverage. The
            // margins allow rasterizer rounding; the native control below
            // verifies they still reject a whole-line bitmap snap.
            if (scroll > 0.06 || press > 0.08 || change(rest, returned) > 0.001 || !neutralCoverage(transparent)
                || change(rest, transparent) > 0.001) {
                std::fprintf(stderr, "FAIL: dpr=%g font=%d scroll=%g press=%g return=%g neutral=%d\n", dpr, pixelSize,
                    scroll, press, change(rest, returned), int(neutralCoverage(transparent)));
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
            std::printf("PASS: dpr=%g font=%d scroll=%g press=%g native-scroll=%g\n", dpr, pixelSize, scroll, press,
                nativeScroll);
        }
    }
    return 0;
}
