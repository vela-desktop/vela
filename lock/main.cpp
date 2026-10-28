// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-lock: Vela's lock screen.
//
// It uses ext-session-lock-v1: the compositor shows only our surfaces (one per
// output) until we unlock, and if this program fails the screen stays locked.
// Like Windows 11: first the big time and date over the wallpaper; a key or a
// click shows the user and the password.
//
// It draws with QPainter into wl_shm buffers, at each output's exact scale
// (fractional-scale-v1 + viewporter). Qt is used only for fonts and images
// ("offscreen" platform); the Wayland connection is ours. The password is
// checked with PAM (the "vela-lock" service, or "login" if not installed) in a
// separate thread: a failure makes you wait a few seconds.

#include "ext-session-lock-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#include "wallpaperblur.h"
#include "defaults.h"
#include "language.h"

#include <QCoreApplication>
#include <QAbstractEventDispatcher>
#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QSocketNotifier>
#include <QSvgRenderer>
#include <QTimer>

#include <security/pam_appl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pwd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// ------------------------------------------------------------- state --

struct Screen {
    uint32_t name = 0;
    wl_output* output = nullptr;
    wl_surface* surface = nullptr;
    ext_session_lock_surface_v1* lockSurface = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractional = nullptr;
    int width = 0; // logical, from the configure
    int height = 0;
    uint32_t scale120 = 120;
    bool configured = false;
    QImage background; // wallpaper already scaled and darkened, for this size
    QImage backgroundBlurred;
    uint32_t blurredScale120 = 0; // blur radius depends on output scale, not only pixel dimensions
};

enum class Mode { Clock, Login, Checking };

wl_display* display = nullptr;
wl_compositor* compositor = nullptr;
wl_shm* shm = nullptr;
wl_seat* seat = nullptr;
wl_keyboard* keyboard = nullptr;
wl_pointer* pointer = nullptr;
ext_session_lock_manager_v1* lockManager = nullptr;
ext_session_lock_v1* lock = nullptr;
wp_viewporter* viewporter = nullptr;
wp_fractional_scale_manager_v1* fractionalManager = nullptr;
std::vector<std::unique_ptr<Screen>> screens;

xkb_context* xkb = nullptr;
xkb_keymap* keymap = nullptr;
xkb_state* xkbState = nullptr;

Mode mode = Mode::Clock;
std::string password;
QString message;
qint64 lastInputMs = 0;
bool unlocked = false;

QString userName;
QString displayName;

void redrawAll();

// --------------------------------------------------------- wallpaper --

QImage wallpaperFor(int width, int height)
{
    const QString path = qEnvironmentVariable("VELA_WALLPAPER", vela::defaults::wallpaper());
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(6, 24, 45));
    QPainter p(&image);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
        QSvgRenderer svg(path);
        if (svg.isValid()) {
            // Fills by cropping the edges, like the shell's wallpaper.
            const QSizeF size = svg.defaultSize().scaled(width, height, Qt::KeepAspectRatioByExpanding);
            svg.render(&p, QRectF((width - size.width()) / 2, (height - size.height()) / 2, size.width(), size.height()));
        }
    } else {
        const QImage source(path);
        if (!source.isNull()) {
            const QImage scaled = source.scaled(width, height, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p.drawImage((width - scaled.width()) / 2, (height - scaled.height()) / 2, scaled);
        }
    }
    return image;
}

// ------------------------------------------------------------ drawing --

void onBufferRelease(void*, wl_buffer* buffer)
{
    wl_buffer_destroy(buffer);
}
const wl_buffer_listener bufferListener { onBufferRelease };

void draw(Screen& screen)
{
    if (!screen.configured || screen.width <= 0 || screen.height <= 0) {
        return;
    }
    const int w = int((int64_t(screen.width) * screen.scale120 + 60) / 120);
    const int h = int((int64_t(screen.height) * screen.scale120 + 60) / 120);
    const int stride = w * 4;
    const size_t size = size_t(stride) * size_t(h);
    const int fd = memfd_create("vela-lock", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, off_t(size)) < 0) {
        return;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return;
    }

    if (screen.background.size() != QSize(w, h)) {
        screen.background = wallpaperFor(w, h);
        screen.backgroundBlurred = {};
    }
    // A scale change can preserve the physical buffer size while changing
    // logical dimensions. In that case the blur must still be regenerated.
    if (screen.backgroundBlurred.isNull() || screen.blurredScale120 != screen.scale120) {
        screen.backgroundBlurred = vela::lock::blurWallpaper(screen.background, screen.scale120);
        screen.blurredScale120 = screen.scale120;
    }
    {
        QImage image(static_cast<uchar*>(data), w, h, stride, QImage::Format_RGB32);
        QPainter p(&image);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        const bool login = mode != Mode::Clock;
        p.drawImage(0, 0, login ? screen.backgroundBlurred : screen.background);
        p.fillRect(QRect(0, 0, w, h), QColor(0, 0, 0, login ? 110 : 60));

        // From here on in logical coordinates.
        const double scale = double(screen.scale120) / 120.0;
        p.scale(scale, scale);
        const double lw = screen.width;
        const double lh = screen.height;
        QFont font = QGuiApplication::font();

        if (!login) {
            const QDateTime now = QDateTime::currentDateTime();
            font.setPixelSize(120);
            font.setWeight(QFont::DemiBold);
            p.setFont(font);
            p.setPen(QColor(255, 255, 255, 240));
            const QRectF timeBox(0, lh * 0.12, lw, 150);
            p.drawText(timeBox, Qt::AlignHCenter | Qt::AlignTop, QLocale().toString(now.time(), QStringLiteral("HH:mm")));
            font.setPixelSize(26);
            font.setWeight(QFont::Medium);
            p.setFont(font);
            const QString date = QLocale().toString(now.date(), QStringLiteral("dddd d MMMM"));
            p.drawText(QRectF(0, timeBox.bottom() + 4, lw, 40), Qt::AlignHCenter | Qt::AlignTop, date);
        } else {
            // User in the center, password below.
            const double cx = lw / 2;
            const double top = lh * 0.30;
            const double avatar = 150;
            QPainterPath circle;
            circle.addEllipse(QPointF(cx, top + avatar / 2), avatar / 2, avatar / 2);
            p.fillPath(circle, QColor(91, 140, 255));
            font.setPixelSize(64);
            font.setWeight(QFont::DemiBold);
            p.setFont(font);
            p.setPen(Qt::white);
            p.drawText(QRectF(cx - avatar / 2, top, avatar, avatar), Qt::AlignCenter, displayName.left(1).toUpper());

            font.setPixelSize(28);
            p.setFont(font);
            const double nameY = top + avatar + 20;
            p.drawText(QRectF(0, nameY, lw, 40), Qt::AlignHCenter | Qt::AlignTop, displayName);

            const QRectF field(cx - 150, nameY + 60, 300, 40);
            QPainterPath fieldPath;
            fieldPath.addRoundedRect(field, 6, 6);
            p.fillPath(fieldPath, QColor(255, 255, 255, 36));
            p.setPen(QPen(QColor(255, 255, 255, 50), 1));
            p.drawPath(fieldPath);
            // The accent line at the bottom, as in Windows 11 fields.
            p.fillRect(QRectF(field.left() + 2, field.bottom() - 2, field.width() - 4, 2), QColor(91, 140, 255));

            font.setPixelSize(15);
            font.setWeight(QFont::Normal);
            p.setFont(font);
            const QRectF textBox = field.adjusted(14, 0, -14, 0);
            if (mode == Mode::Checking) {
                p.setPen(QColor(255, 255, 255, 170));
                p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, QCoreApplication::translate("Lock", "Signing in…"));
            } else if (password.empty()) {
                p.setPen(QColor(255, 255, 255, 140));
                p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, QCoreApplication::translate("Lock", "Password"));
            } else {
                // One dot per character (multibyte UTF-8 characters count as
                // one).
                const auto chars = std::count_if(password.begin(), password.end(),
                    [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
                p.setPen(Qt::white);
                p.setBrush(Qt::white);
                const double dot = 7;
                for (qsizetype i = 0; i < std::min<qsizetype>(chars, 32); ++i) {
                    p.drawEllipse(QPointF(textBox.left() + dot / 2 + i * (dot + 5), textBox.center().y()), dot / 2, dot / 2);
                }
            }
            if (!message.isEmpty()) {
                p.setPen(QColor(255, 255, 255, 220));
                p.drawText(QRectF(0, field.bottom() + 16, lw, 30), Qt::AlignHCenter | Qt::AlignTop, message);
            }
        }
    }
    munmap(data, size);

    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, int32_t(size));
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buffer, &bufferListener, nullptr);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (screen.viewport) {
        wp_viewport_set_destination(screen.viewport, screen.width, screen.height);
    }
    wl_surface_attach(screen.surface, buffer, 0, 0);
    wl_surface_damage_buffer(screen.surface, 0, 0, w, h);
    wl_surface_commit(screen.surface);
}

void redrawAll()
{
    for (auto& screen : screens) {
        draw(*screen);
    }
    wl_display_flush(display);
}

// ----------------------------------------------------------------- PAM --

int pamConversation(int count, const pam_message** messages, pam_response** responses, void* data)
{
    const auto* secret = static_cast<const std::string*>(data);
    auto* replies = static_cast<pam_response*>(calloc(size_t(count), sizeof(pam_response)));
    if (!replies) {
        return PAM_BUF_ERR;
    }
    for (int i = 0; i < count; ++i) {
        if (messages[i]->msg_style == PAM_PROMPT_ECHO_OFF || messages[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            replies[i].resp = strdup(secret->c_str());
        }
    }
    *responses = replies;
    return PAM_SUCCESS;
}

// In PAM's thread; the result goes back to the main thread.
void authenticate(std::string secret)
{
    const char* service = access("/etc/pam.d/vela-lock", F_OK) == 0 ? "vela-lock" : "login";
    const pam_conv conversation { pamConversation, &secret };
    pam_handle_t* handle = nullptr;
    const std::string user = userName.toStdString();
    bool ok = pam_start(service, user.c_str(), &conversation, &handle) == PAM_SUCCESS
        && pam_authenticate(handle, 0) == PAM_SUCCESS;
    if (handle) {
        if (ok) {
            pam_setcred(handle, PAM_REFRESH_CRED);
        }
        pam_end(handle, ok ? PAM_SUCCESS : PAM_AUTH_ERR);
    }
    std::fill(secret.begin(), secret.end(), '\0');
    QMetaObject::invokeMethod(
        qApp,
        [ok] {
            if (ok) {
                unlocked = true;
                ext_session_lock_v1_unlock_and_destroy(lock);
                wl_display_roundtrip(display);
                QGuiApplication::quit();
                return;
            }
            mode = Mode::Login;
            message = QCoreApplication::translate("Lock", "The password is incorrect. Try again.");
            redrawAll();
        },
        Qt::QueuedConnection);
}

void submit()
{
    if (mode != Mode::Login || password.empty()) {
        return;
    }
    mode = Mode::Checking;
    message.clear();
    std::string secret = std::move(password);
    password.clear();
    redrawAll();
    std::thread(authenticate, std::move(secret)).detach();
}

// ------------------------------------------------------------- input --

void activity()
{
    lastInputMs = QDateTime::currentMSecsSinceEpoch();
}

void showLogin()
{
    if (mode == Mode::Clock) {
        mode = Mode::Login;
        message.clear();
        redrawAll();
    }
}

void onKeymap(void*, wl_keyboard*, uint32_t format, int fd, uint32_t size)
{
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED) {
        return;
    }
    xkb_keymap* fresh = xkb_keymap_new_from_string(xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!fresh) {
        return;
    }
    xkb_state_unref(xkbState);
    xkb_keymap_unref(keymap);
    keymap = fresh;
    xkbState = xkb_state_new(keymap);
}

void onKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkbState) {
        return;
    }
    activity();
    const xkb_keycode_t code = key + 8;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkbState, code);
    if (mode == Mode::Checking) {
        return;
    }
    if (mode == Mode::Clock) {
        showLogin();
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_Escape || sym == XKB_KEY_space) {
            return; // only to make the password appear
        }
    }
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        submit();
        return;
    case XKB_KEY_Escape:
        std::fill(password.begin(), password.end(), '\0');
        password.clear();
        message.clear();
        mode = Mode::Clock;
        redrawAll();
        return;
    case XKB_KEY_BackSpace:
        // Removes the last whole UTF-8 character.
        while (!password.empty()) {
            const unsigned char last = static_cast<unsigned char>(password.back());
            password.pop_back();
            if ((last & 0xC0) != 0x80) {
                break;
            }
        }
        redrawAll();
        return;
    default:
        break;
    }
    char text[16] {};
    const int length = xkb_state_key_get_utf8(xkbState, code, text, sizeof(text));
    if (length > 0 && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f && password.size() < 512) {
        password.append(text, size_t(length));
        message.clear();
        redrawAll();
    }
}

void onModifiers(void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    if (xkbState) {
        xkb_state_update_mask(xkbState, depressed, latched, locked, 0, 0, group);
    }
}

void onEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) { }
void onLeave(void*, wl_keyboard*, uint32_t, wl_surface*) { }
void onRepeat(void*, wl_keyboard*, int32_t, int32_t) { }
const wl_keyboard_listener keyboardListener { onKeymap, onEnter, onLeave, onKey, onModifiers, onRepeat };

void onPointerEnter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) { }
void onPointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) { }
void onPointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) { activity(); }
void onPointerButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t state)
{
    activity();
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        showLogin();
    }
}
void onPointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) { }
const wl_pointer_listener pointerListener { onPointerEnter, onPointerLeave, onPointerMotion, onPointerButton, onPointerAxis };

void onSeatCapabilities(void*, wl_seat*, uint32_t capabilities)
{
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &keyboardListener, nullptr);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointerListener, nullptr);
    }
}
void onSeatName(void*, wl_seat*, const char*) { }
const wl_seat_listener seatListener { onSeatCapabilities, onSeatName };

// ------------------------------------------------------------ surfaces --

void onLockSurfaceConfigure(void* data, ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t width,
    uint32_t height)
{
    auto* screen = static_cast<Screen*>(data);
    ext_session_lock_surface_v1_ack_configure(surface, serial);
    screen->width = int(width);
    screen->height = int(height);
    screen->configured = true;
    draw(*screen);
}
const ext_session_lock_surface_v1_listener lockSurfaceListener { onLockSurfaceConfigure };

void onPreferredScale(void* data, wp_fractional_scale_v1*, uint32_t scale)
{
    auto* screen = static_cast<Screen*>(data);
    if (screen->scale120 != scale) {
        screen->scale120 = scale;
        draw(*screen);
    }
}
const wp_fractional_scale_v1_listener fractionalListener { onPreferredScale };

void createLockSurface(Screen& screen)
{
    if (!lock || screen.lockSurface) {
        return;
    }
    screen.surface = wl_compositor_create_surface(compositor);
    if (viewporter) {
        screen.viewport = wp_viewporter_get_viewport(viewporter, screen.surface);
    }
    if (fractionalManager) {
        screen.fractional = wp_fractional_scale_manager_v1_get_fractional_scale(fractionalManager, screen.surface);
        wp_fractional_scale_v1_add_listener(screen.fractional, &fractionalListener, &screen);
    }
    screen.lockSurface = ext_session_lock_v1_get_lock_surface(lock, screen.surface, screen.output);
    ext_session_lock_surface_v1_add_listener(screen.lockSurface, &lockSurfaceListener, &screen);
}

void destroyScreen(Screen& screen)
{
    if (screen.lockSurface) {
        ext_session_lock_surface_v1_destroy(screen.lockSurface);
    }
    if (screen.fractional) {
        wp_fractional_scale_v1_destroy(screen.fractional);
    }
    if (screen.viewport) {
        wp_viewport_destroy(screen.viewport);
    }
    if (screen.surface) {
        wl_surface_destroy(screen.surface);
    }
    wl_output_destroy(screen.output);
}

void onLocked(void*, ext_session_lock_v1*) { }
void onFinished(void*, ext_session_lock_v1*)
{
    // The compositor didn't give us the lock (another program already has it).
    if (!unlocked) {
        fprintf(stderr, "vela-lock: the compositor refused the lock\n");
        QGuiApplication::exit(1);
    }
}
const ext_session_lock_v1_listener lockListener { onLocked, onFinished };

// ------------------------------------------------------------ registry --

void onGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name)) {
        compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (!strcmp(interface, wl_seat_interface.name) && !seat) {
        seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 4u)));
        wl_seat_add_listener(seat, &seatListener, nullptr);
    } else if (!strcmp(interface, ext_session_lock_manager_v1_interface.name)) {
        lockManager = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1));
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        fractionalManager = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
    } else if (!strcmp(interface, wl_output_interface.name)) {
        auto screen = std::make_unique<Screen>();
        screen->name = name;
        screen->output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 1));
        createLockSurface(*screen); // output plugged in while locked
        screens.push_back(std::move(screen));
    }
}

void onGlobalRemove(void*, wl_registry*, uint32_t name)
{
    for (auto it = screens.begin(); it != screens.end(); ++it) {
        if ((*it)->name == name) {
            destroyScreen(**it);
            screens.erase(it);
            return;
        }
    }
}
const wl_registry_listener registryListener { onGlobal, onGlobalRemove };

} // namespace

int main(int argc, char* argv[])
{
    // Qt only for fonts and images: no Qt window.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    vela::language::install(QStringLiteral("vela-lock")); // draws every frame: nothing to retranslate

    if (const passwd* pw = getpwuid(getuid())) {
        userName = QString::fromLocal8Bit(pw->pw_name);
        displayName = QString::fromLocal8Bit(pw->pw_gecos).section(QLatin1Char(','), 0, 0);
        if (displayName.isEmpty()) {
            displayName = userName;
        }
    }

    display = wl_display_connect(nullptr);
    if (!display) {
        fprintf(stderr, "vela-lock: no Wayland compositor\n");
        return 1;
    }
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, nullptr);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !lockManager) {
        fprintf(stderr, "vela-lock: the compositor doesn't offer ext-session-lock-v1\n");
        return 1;
    }

    lock = ext_session_lock_manager_v1_lock(lockManager);
    ext_session_lock_v1_add_listener(lock, &lockListener, nullptr);
    for (auto& screen : screens) {
        createLockSurface(*screen);
    }
    wl_display_roundtrip(display);

    // Wayland events inside Qt's loop.
    QSocketNotifier notifier(wl_display_get_fd(display), QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, [] {
        if (wl_display_dispatch(display) < 0) {
            QGuiApplication::exit(1);
        }
    });
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, [] {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
    });

    // The clock changes every minute; the password disappears after 30 s of
    // nothing.
    QTimer tick;
    QString shownMinute;
    QObject::connect(&tick, &QTimer::timeout, [&shownMinute] {
        if (mode == Mode::Login && QDateTime::currentMSecsSinceEpoch() - lastInputMs > 30000) {
            std::fill(password.begin(), password.end(), '\0');
            password.clear();
            message.clear();
            mode = Mode::Clock;
            redrawAll();
            return;
        }
        const QString minute = QTime::currentTime().toString(QStringLiteral("HH:mm"));
        if (mode == Mode::Clock && minute != shownMinute) {
            shownMinute = minute;
            redrawAll();
        }
    });
    tick.start(1000);

    const int result = app.exec();
    wl_display_disconnect(display);
    return unlocked ? 0 : result;
}
