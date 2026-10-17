// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-polkit-prompt: Vela's User Account Control dialog
// (docs/polkit-agent.md §6). vela-polkit-agent starts it, one per request; it
// reads the request from stdin and writes the answers to stdout.
//
// Two kinds of layer-shell surfaces on the "overlay" layer: the dialog, full
// screen with its veil, on the output the compositor picks (the one under the
// pointer), with the keyboard exclusively; and a veil on each of the other
// outputs. All of them take the pointer: nothing underneath can be touched
// until the user answers.

#include "controls.h"
#include "language.h"
#include "request.h"

#include <LayerShellQt/Window>

#include <QGuiApplication>
#include <QHash>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QtQml/QQmlExtensionPlugin>

#include <cstdio>
#include <cstring>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

using LayerWindow = LayerShellQt::Window;

namespace {

// The fade-out duration (Theme.normal) plus a margin for the last frame, at
// Vela's durations (times Style.animationDuration).
constexpr int fadeOutMs = 200 + 60;

void configure(QWindow* window, bool dialog, QScreen* screen)
{
    LayerWindow* layer = LayerWindow::get(window);
    layer->setLayer(LayerWindow::LayerOverlay);
    layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorTop) | LayerWindow::AnchorBottom
        | LayerWindow::AnchorLeft | LayerWindow::AnchorRight);
    layer->setExclusiveZone(-1); // the whole output, taskbar included
    layer->setScope(dialog ? QStringLiteral("vela-polkit") : QStringLiteral("vela-polkit-veil"));
    layer->setKeyboardInteractivity(
        dialog ? LayerWindow::KeyboardInteractivityExclusive : LayerWindow::KeyboardInteractivityNone);
    if (dialog) {
        layer->setWantsToBeOnActiveScreen(true); // the compositor picks it
    } else {
        layer->setScreen(screen);
    }
}

class Windows : public QObject {
public:
    explicit Windows(QQmlEngine& engine)
        : m_engine(engine)
        , m_dialogComponent(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Vela/Polkit/Dialog.qml")))
        , m_veilComponent(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Vela/Polkit/Veil.qml")))
    {
        connect(qGuiApp, &QGuiApplication::screenAdded, this, &Windows::updateVeils);
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
            if (QWindow* veil = m_veils.take(screen)) {
                veil->deleteLater();
            }
            updateVeils();
        });
    }

    bool show()
    {
        m_dialog = create(m_dialogComponent);
        if (!m_dialog) {
            return false;
        }
        configure(m_dialog, true, nullptr);
        // The compositor picks the output: we know it when the surface
        // enters it. Then the veils go on the others.
        connect(m_dialog, &QWindow::screenChanged, this, &Windows::updateVeils);
        m_dialog->show();
        updateVeils();
        return true;
    }

private:
    QQuickWindow* create(QQmlComponent& component)
    {
        QObject* object = component.create();
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning("vela-polkit-prompt: %s", qPrintable(component.errorString()));
            delete object;
            return nullptr;
        }
        return window;
    }

    void updateVeils()
    {
        if (!m_dialog) {
            return;
        }
        QScreen* dialogScreen = m_dialog->screen();
        if (QWindow* veil = m_veils.take(dialogScreen)) {
            veil->deleteLater();
        }
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen == dialogScreen || m_veils.contains(screen)) {
                continue;
            }
            QQuickWindow* veil = create(m_veilComponent);
            if (!veil) {
                return;
            }
            configure(veil, false, screen);
            veil->show();
            m_veils.insert(screen, veil);
        }
    }

    QQmlEngine& m_engine;
    QQmlComponent m_dialogComponent;
    QQmlComponent m_veilComponent;
    QPointer<QQuickWindow> m_dialog;
    QHash<QScreen*, QWindow*> m_veils;
};

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 1 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h"))) {
        std::printf("Usage: vela-polkit-prompt\n"
                    "Vela's User Account Control dialog. Started by vela-polkit-agent, which\n"
                    "describes the request on stdin and reads the answers from stdout\n"
                    "(docs/polkit-agent.md, section 5).\n");
        return 0;
    }
    // Always Wayland: the overlay layer exists only there.
    qputenv("QT_QPA_PLATFORM", "wayland");
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("vela-polkit-prompt"));
    app.setDesktopFileName(QStringLiteral("vela-polkit-prompt"));
    // Windows close when we say so, not when they disappear.
    app.setQuitOnLastWindowClosed(false);

    Request request(&app);
    QQmlEngine engine;
    vela::language::install(QStringLiteral("vela-polkit-prompt"), [&engine] { engine.retranslate(); });
    const Appearance* style = vela::controls::install(engine, Appearance::Mode::Shell);

    Windows windows(engine);
    bool shown = false;
    QObject::connect(&request, &Request::shownChanged, &app, [&] {
        if (!shown && request.shown()) {
            shown = true;
            if (!windows.show()) {
                request.cancel();
            }
        }
    });
    QObject::connect(&request, &Request::closingChanged, &app, [&] {
        // Without windows (closed before "show") exit at once.
        const qreal duration = style->animationEffects() ? style->animationDuration() : 1.0;
        QTimer::singleShot(shown ? qRound(fadeOutMs * duration) : 0, &app, [] { QCoreApplication::exit(0); });
    });
    request.listen();
    return app.exec();
}
