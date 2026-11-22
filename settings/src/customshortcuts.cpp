// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "customshortcuts.h"

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

extern "C" {
#include "custom.h"
#include "shortcuts.h"
#include "table.h"
}

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QSaveFile>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <memory>

namespace {

constexpr qsizetype MaxFileBytes = 1024 * 1024;

std::optional<QByteArray> readShortcuts(const QString& path)
{
    QFile file(path);
    if (!file.exists()) {
        return QByteArray();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray contents = file.read(MaxFileBytes + 1);
    if (file.error() != QFile::NoError || contents.size() > MaxFileBytes) {
        return std::nullopt;
    }
    return contents;
}

struct KeymapDeleter {
    void operator()(xkb_keymap* keymap) const { xkb_keymap_unref(keymap); }
};
struct StateDeleter {
    void operator()(xkb_state* state) const { xkb_state_unref(state); }
};

// The keymap of the layouts in use, all of them, the first active.
std::unique_ptr<xkb_keymap, KeymapDeleter> keymapFor(const QVariantList& layouts)
{
    QStringList names;
    QStringList variants;
    for (const QVariant& value : layouts) {
        const QVariantMap layout = value.toMap();
        names << layout.value(QStringLiteral("layout")).toString();
        variants << layout.value(QStringLiteral("variant")).toString();
    }
    if (names.isEmpty()) {
        names << QStringLiteral("us");
        variants << QString();
    }
    const QByteArray layout = names.join(u',').toUtf8();
    const QByteArray variant = variants.join(u',').toUtf8();
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_rule_names rules {};
    rules.rules = "evdev";
    rules.model = "pc105";
    rules.layout = layout.constData();
    rules.variant = variant.constData();
    xkb_keymap* keymap = context ? xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
    if (!keymap && context) {
        const xkb_rule_names us { "evdev", "pc105", "us", nullptr, nullptr };
        keymap = xkb_keymap_new_from_names(context, &us, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    xkb_context_unref(context);
    return std::unique_ptr<xkb_keymap, KeymapDeleter>(keymap);
}

bool isModifier(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_Hyper_L:
    case XKB_KEY_Hyper_R:
    case XKB_KEY_ISO_Level3_Shift:
    case XKB_KEY_ISO_Level5_Shift:
    case XKB_KEY_Caps_Lock:
    case XKB_KEY_Num_Lock:
    case XKB_KEY_Shift_Lock: return true;
    default: return false;
    }
}

QString keysymText(xkb_keysym_t sym)
{
    char text[16] {};
    const int length = xkb_keysym_to_utf8(xkb_keysym_to_upper(sym), text, sizeof(text));
    const QString out = length > 1 ? QString::fromUtf8(text) : QString();
    return out.size() == 1 && out.at(0).isPrint() && !out.at(0).isSpace() ? out : QString();
}

// The name of a key without a printable symbol, as keyboards print it.
QString keyName(xkb_keysym_t sym)
{
    static const std::pair<xkb_keysym_t, const char*> names[] = {
        { XKB_KEY_Return, "Enter" },
        { XKB_KEY_KP_Enter, "Num Enter" },
        { XKB_KEY_Escape, "Esc" },
        { XKB_KEY_space, "Space" },
        { XKB_KEY_Tab, "Tab" },
        { XKB_KEY_BackSpace, "Backspace" },
        { XKB_KEY_Delete, "Del" },
        { XKB_KEY_Insert, "Ins" },
        { XKB_KEY_Home, "Home" },
        { XKB_KEY_End, "End" },
        { XKB_KEY_Prior, "Page Up" },
        { XKB_KEY_Next, "Page Down" },
        { XKB_KEY_Left, "←" },
        { XKB_KEY_Right, "→" },
        { XKB_KEY_Up, "↑" },
        { XKB_KEY_Down, "↓" },
        { XKB_KEY_Print, "PrtSc" },
        { XKB_KEY_Pause, "Pause" },
        { XKB_KEY_Scroll_Lock, "Scroll Lock" },
        { XKB_KEY_Menu, "Menu" },
    };
    for (const auto& [key, name] : names) {
        if (key == sym) {
            return QString::fromUtf8(name);
        }
    }
    char text[64] {};
    if (xkb_keysym_get_name(sym, text, sizeof(text)) <= 0) {
        return {};
    }
    QString name = QString::fromLatin1(text);
    if (name.startsWith(QLatin1String("XF86"))) {
        name = name.mid(4);
    } else if (name.startsWith(QLatin1String("KP_"))) {
        name = QStringLiteral("Num ") + name.mid(3);
    }
    return name;
}

vela_modes tableModes(const CustomShortcuts::Modes& modes)
{
    vela_modes out {};
    out.color_filter_shortcut = modes.colorFilterShortcut;
    // The magnifier may be on when the shortcut is used: its keys are
    // taken. Alt+Tab's Esc isn't (the switcher is a moment).
    out.magnifier = true;
    out.numbers = vela_super_numbers_parse(modes.superNumberKeys.toUtf8().constData());
    return out;
}

const wl_registry_listener registryListener {
    [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto** manager = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1**>(data);
        if (!*manager && qstrcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0) {
            *manager = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
                wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
            Q_UNUSED(version);
        }
    },
    [](void*, wl_registry*, uint32_t) {},
};

} // namespace

CustomShortcuts::CustomShortcuts(
    AppsSource apps, LayoutsSource layouts, ModesSource modes, Written written, QObject* parent)
    : QObject(parent)
    , m_apps(std::move(apps))
    , m_layouts(std::move(layouts))
    , m_modes(std::move(modes))
    , m_written(std::move(written))
{
    reload();
}

CustomShortcuts::~CustomShortcuts()
{
    stopRecording();
    if (m_manager) {
        zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(m_manager);
    }
    if (m_registry) {
        wl_registry_destroy(m_registry);
    }
}

QString CustomShortcuts::path()
{
    char out[4096];
    return vela_custom_path(out, sizeof(out)) ? QString::fromUtf8(out) : QString();
}

void CustomShortcuts::reload()
{
    m_lines.clear();
    m_snapshot = readShortcuts(path());
    if (m_snapshot) {
        const QList<QByteArray> lines = m_snapshot->split('\n');
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const QByteArray& raw = lines.at(i);
            if (i == lines.size() - 1 && raw.isEmpty()) {
                break; // the last newline
            }
            Line line;
            line.text = QString::fromUtf8(raw);
            vela_custom_line parsed {};
            char combo[128];
            if (vela_custom_line_parse(raw.constData(), size_t(raw.size()), &parsed)
                && vela_combo_format(&parsed.combo, combo, sizeof(combo))) {
                line.combo = QString::fromLatin1(combo);
                line.kind = QString::fromLatin1(vela_custom_kind_name(parsed.kind));
                line.target = QString::fromUtf8(parsed.target, qsizetype(parsed.target_length));
            }
            m_lines.append(line);
        }
    }
    refreshItems();
}

void CustomShortcuts::refreshItems()
{
    const QVariantList layouts = m_layouts ? m_layouts() : QVariantList();
    const QVariantList installed = m_apps ? m_apps() : QVariantList();
    m_items.clear();
    for (const Line& line : std::as_const(m_lines)) {
        if (line.combo.isEmpty()) {
            continue;
        }
        QVariantMap item {
            { QStringLiteral("combo"), line.combo },
            { QStringLiteral("label"), labelFor(line.combo, layouts) },
            { QStringLiteral("kind"), line.kind },
            { QStringLiteral("target"), line.target },
            { QStringLiteral("name"), line.target },
            { QStringLiteral("icon"),
                line.kind == QLatin1String("app")           ? QStringLiteral("application-x-executable")
                    : line.kind == QLatin1String("command") ? QStringLiteral("utilities-terminal")
                                                            : QStringLiteral("internet-web-browser") },
        };
        if (line.kind == QLatin1String("app")) {
            for (const QVariant& value : std::as_const(installed)) {
                const QVariantMap app = value.toMap();
                if (app.value(QStringLiteral("id")).toString() == line.target) {
                    item.insert(QStringLiteral("name"), app.value(QStringLiteral("name")));
                    item.insert(QStringLiteral("icon"), app.value(QStringLiteral("icon")));
                }
            }
        }
        m_items.append(item);
    }
    emit changed();
}

QString CustomShortcuts::labelFor(const QString& combo, const QVariantList& layouts)
{
    vela_combo parsed {};
    if (!vela_combo_parse(combo.toUtf8().constData(), &parsed)) {
        return combo;
    }
    QStringList parts;
    if (parsed.mods & VELA_MOD_SUPER) {
        parts << QStringLiteral("Win");
    }
    if (parsed.mods & VELA_MOD_CTRL) {
        parts << QStringLiteral("Ctrl");
    }
    if (parsed.mods & VELA_MOD_ALT) {
        parts << QStringLiteral("Alt");
    }
    if (parsed.mods & VELA_MOD_SHIFT) {
        parts << QStringLiteral("Shift");
    }
    QString key;
    if (parsed.kind == VELA_KEY_SYM) {
        key = keysymText(parsed.key);
    } else {
        // What the first layout prints on that key: its digit on the digit
        // row (AZERTY's are on the second level), its symbol, or its name.
        const auto keymap = keymapFor(layouts.mid(0, 1));
        const xkb_keycode_t code = parsed.key + 8;
        const bool digitRow = parsed.key >= KEY_1 && parsed.key <= KEY_0;
        for (xkb_level_index_t level = 0; keymap && level < 2 && key.isEmpty(); ++level) {
            const xkb_keysym_t* syms = nullptr;
            if (xkb_keymap_key_get_syms_by_level(keymap.get(), code, 0, level, &syms) < 1) {
                continue;
            }
            const QString text = keysymText(syms[0]);
            if (digitRow ? (text.size() == 1 && text.at(0).isDigit()) : level == 0) {
                key = text.isEmpty() ? keyName(syms[0]) : text;
            }
        }
        if (key.isEmpty()) {
            char name[64];
            key = vela_combo_format(&parsed, name, sizeof(name)) ? QString::fromLatin1(name).section(u'+', -1) : combo;
        }
    }
    parts << (key.isEmpty() ? combo.section(u'+', -1) : key);
    return parts.join(u'+');
}

QString CustomShortcuts::label(const QString& combo) const
{
    return labelFor(combo, m_layouts ? m_layouts() : QVariantList());
}

QVariantMap CustomShortcuts::press(int scanCode, int modifiers) const
{
    if (m_keyboard) {
        return pressWithState(scanCode, m_state);
    }
    const QVariantList layouts = m_layouts ? m_layouts() : QVariantList();
    const auto keymap = keymapFor(layouts);
    if (!keymap || scanCode < 8) {
        return { { QStringLiteral("state"), QStringLiteral("modifier") } };
    }
    std::unique_ptr<xkb_state, StateDeleter> state(xkb_state_new(keymap.get()));
    xkb_mod_mask_t mask = 0;
    const auto add = [&](Qt::KeyboardModifier qt, const char* name) {
        const xkb_mod_index_t index = xkb_keymap_mod_get_index(keymap.get(), name);
        if ((modifiers & qt) && index != XKB_MOD_INVALID) {
            mask |= 1u << index;
        }
    };
    add(Qt::ControlModifier, XKB_MOD_NAME_CTRL);
    add(Qt::AltModifier, XKB_MOD_NAME_ALT);
    add(Qt::ShiftModifier, XKB_MOD_NAME_SHIFT);
    add(Qt::MetaModifier, XKB_MOD_NAME_LOGO);
    xkb_state_update_mask(state.get(), mask, 0, 0, 0, 0, 0);
    return pressWithState(scanCode, state.get());
}

QVariantMap CustomShortcuts::pressWithState(int scanCode, xkb_state* state) const
{
    if (!state || scanCode < 8 || isModifier(xkb_state_key_get_one_sym(state, scanCode))) {
        return { { QStringLiteral("state"), QStringLiteral("modifier") } };
    }
    const QVariantList layouts = m_layouts ? m_layouts() : QVariantList();
    vela_press pressed {};
    vela_press_from_state(state, xkb_keycode_t(scanCode), &pressed);
    const vela_combo combo = vela_combo_from_press(&pressed);
    char text[128];
    if (!vela_combo_format(&combo, text, sizeof(text))) {
        return { { QStringLiteral("state"), QStringLiteral("refused") },
            { QStringLiteral("reason"), tr("This key can't be used for a shortcut.") } };
    }
    const QString comboText = QString::fromLatin1(text);
    QVariantMap out {
        { QStringLiteral("state"), QStringLiteral("ok") },
        { QStringLiteral("combo"), comboText },
        { QStringLiteral("label"), labelFor(comboText, layouts) },
        { QStringLiteral("warning"), !(pressed.mods & VELA_MOD_SUPER) },
        { QStringLiteral("existing"), -1 },
    };
    // Alone, or with Shift only, a key types: only the keys that type
    // nothing are allowed so (F13–F24, Calculator, Mail…).
    const bool plain = (pressed.mods & ~VELA_MOD_SHIFT) == 0;
    const bool special = (pressed.keycode >= KEY_F13 && pressed.keycode <= KEY_F24)
        || (vela_press_is_positional(&pressed) && (pressed.sym & 0xffff0000) == 0x10080000);
    const vela_modes modes = tableModes(m_modes ? m_modes() : Modes());
    const vela_binding* taken = vela_binding_taken(&pressed, &modes);
    if (taken) {
        out.insert(QStringLiteral("state"), QStringLiteral("refused"));
        out.insert(QStringLiteral("reason"),
            taken->stage == VELA_STAGE_NORMAL ? tr("Vela already uses this combination.")
                                              : tr("This combination is reserved for Vela."));
    } else if (plain && !special) {
        out.insert(QStringLiteral("state"), QStringLiteral("refused"));
        out.insert(QStringLiteral("reason"), tr("Add Ctrl, Alt or Win: alone or with Shift, this key types."));
    }
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value(QStringLiteral("combo")).toString() == comboText) {
            out.insert(QStringLiteral("existing"), int(i));
        }
    }
    out.insert(QStringLiteral("warning"),
        out.value(QStringLiteral("state")) == QLatin1String("ok") && !(pressed.mods & VELA_MOD_SUPER));
    return out;
}

QVariantList CustomShortcuts::apps(const QString& filter) const
{
    QVariantList out;
    const QVariantList all = m_apps ? m_apps() : QVariantList();
    for (const QVariant& value : all) {
        const QVariantMap app = value.toMap();
        if (filter.isEmpty() || app.value(QStringLiteral("name")).toString().contains(filter, Qt::CaseInsensitive)
            || app.value(QStringLiteral("keys")).toString().contains(filter, Qt::CaseInsensitive)) {
            out.append(app);
        }
    }
    std::sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
        return QString::localeAwareCompare(a.toMap().value(QStringLiteral("name")).toString(),
                   b.toMap().value(QStringLiteral("name")).toString())
            < 0;
    });
    return out;
}

QString CustomShortcuts::targetProblem(const QString& kind, const QString& target) const
{
    const QByteArray bytes = target.toUtf8();
    if (target.trimmed().isEmpty()) {
        return kind == QLatin1String("app")    ? tr("Choose an app.")
            : kind == QLatin1String("command") ? tr("Write a command.")
                                               : tr("Write a link or choose a file.");
    }
    if (bytes.size() > VELA_CUSTOM_LINE_MAX - 128) {
        return tr("Too long.");
    }
    vela_custom_kind parsed = VELA_CUSTOM_COMMAND;
    if (kind == QLatin1String("app")) {
        parsed = VELA_CUSTOM_APP;
    } else if (kind == QLatin1String("open")) {
        parsed = VELA_CUSTOM_OPEN;
    }
    if (!vela_custom_target_valid(parsed, bytes.constData(), size_t(bytes.size()))) {
        return kind == QLatin1String("open") ? tr("Write a link such as https://example.org, or a file's full path.")
                                             : tr("It can't hold line breaks or tabs, nor start or end with a space.");
    }
    return {};
}

bool CustomShortcuts::write(const QList<Line>& lines)
{
    const QString file = path();
    if (file.isEmpty() || !QDir().mkpath(QFileInfo(file).absolutePath())) {
        return false;
    }
    QLockFile lock(file + QStringLiteral(".lock"));
    if (!lock.tryLock()) {
        return false;
    }
    if (!m_snapshot || readShortcuts(file) != m_snapshot) {
        reload();
        return false;
    }
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    QByteArray text;
    bool header = false;
    for (const Line& line : lines) {
        header = header || line.text.startsWith(u'#');
    }
    if (!header) {
        text += "# Vela's custom shortcuts (Settings > Keyboard > Custom shortcuts)\n";
    }
    for (const Line& line : lines) {
        text += (line.combo.isEmpty() ? line.text : line.combo + u'=' + line.kind + u' ' + line.target).toUtf8();
        text += '\n';
    }
    if (text.size() > MaxFileBytes || out.write(text) != text.size() || !out.commit()) {
        return false;
    }
    if (m_written) {
        m_written();
    }
    return true;
}

bool CustomShortcuts::save(int index, const QString& combo, const QString& kind, const QString& target)
{
    vela_combo parsed {};
    char normalized[128];
    if (!vela_combo_parse(combo.toUtf8().constData(), &parsed)
        || !vela_combo_format(&parsed, normalized, sizeof(normalized)) || !targetProblem(kind, target).isEmpty()
        || (kind != QLatin1String("app") && kind != QLatin1String("command") && kind != QLatin1String("open"))) {
        return false;
    }
    const QString text = QString::fromLatin1(normalized);
    const qsizetype replaced = lineOf(index);
    QList<Line> lines;
    bool placed = false;
    for (qsizetype i = 0; i < m_lines.size(); ++i) {
        Line line = m_lines.at(i);
        if (i == replaced || (!line.combo.isEmpty() && line.combo == text)) {
            if (placed) {
                continue; // the other line with that combination goes
            }
            line = Line { {}, text, kind, target.trimmed() };
            placed = true;
        }
        lines.append(line);
    }
    if (!placed) {
        lines.append(Line { {}, text, kind, target.trimmed() });
    }
    if (!write(lines)) {
        return false;
    }
    reload();
    return true;
}

qsizetype CustomShortcuts::lineOf(int index) const
{
    if (index < 0) {
        return -1;
    }
    qsizetype item = 0;
    for (qsizetype i = 0; i < m_lines.size(); ++i) {
        if (m_lines.at(i).combo.isEmpty()) {
            continue;
        }
        if (item == index) {
            return i;
        }
        ++item;
    }
    return -1;
}

bool CustomShortcuts::remove(int index)
{
    const qsizetype removed = lineOf(index);
    if (removed < 0) {
        return false;
    }
    QList<Line> lines = m_lines;
    lines.removeAt(removed);
    if (!write(lines)) {
        return false;
    }
    reload();
    return true;
}

void CustomShortcuts::bindManager()
{
    if (m_registry) {
        return;
    }
    auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    wl_display* display = wayland ? wayland->display() : nullptr;
    if (!display) {
        return;
    }
    m_registry = wl_display_get_registry(display);
    wl_registry_add_listener(m_registry, &registryListener, &m_manager);
    wl_display_roundtrip(display);
}

void CustomShortcuts::startRecording(QWindow* window)
{
    stopRecording();
    bindManager();
    auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    auto* native = window ? window->nativeInterface<QNativeInterface::Private::QWaylandWindow>() : nullptr;
    wl_surface* surface = native ? native->surface() : nullptr;
    wl_seat* seat = wayland ? wayland->seat() : nullptr;
    if (seat && wayland->keyboard()) {
        // Qt exposes scan codes, but the active group and custom keymap come from the seat.
        static const wl_keyboard_listener listener {
            [](void* data, wl_keyboard*, uint32_t format, int fd, uint32_t size) {
                auto** state = static_cast<xkb_state**>(data);
                xkb_state_unref(*state);
                *state = nullptr;
                if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
                    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (map != MAP_FAILED) {
                        auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                        auto* keymap = context ? xkb_keymap_new_from_buffer(context, static_cast<const char*>(map),
                                                     size - 1, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)
                                               : nullptr;
                        *state = keymap ? xkb_state_new(keymap) : nullptr;
                        xkb_keymap_unref(keymap);
                        xkb_context_unref(context);
                        munmap(map, size);
                    }
                }
                close(fd);
            },
            [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {},
            [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
            [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) {},
            [](void* data, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked,
                uint32_t group) {
                if (auto* state = *static_cast<xkb_state**>(data)) {
                    xkb_state_update_mask(state, depressed, latched, locked, 0, 0, group);
                }
            },
            [](void*, wl_keyboard*, int32_t, int32_t) {},
        };
        m_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(m_keyboard, &listener, &m_state);
        wl_display_roundtrip(wayland->display());
    }
    if (!m_manager || !surface || !seat) {
        return; // Win still acts while recording: the other keys are recorded
    }
    m_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m_manager, surface, seat);
    emit recordingChanged();
}

void CustomShortcuts::stopRecording()
{
    if (m_keyboard) {
        wl_keyboard_release(m_keyboard);
        m_keyboard = nullptr;
        xkb_state_unref(m_state);
        m_state = nullptr;
    }
    if (!m_inhibitor) {
        return;
    }
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_inhibitor);
    m_inhibitor = nullptr;
    if (auto* wayland = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr) {
        wl_display_flush(wayland->display());
    }
    emit recordingChanged();
}
