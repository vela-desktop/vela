// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keyboardpreview.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QVariantMap>

#include <array>
#include <cstdlib>
#include <cstring>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

namespace {

// A key of the drawing, in units of a key's width.
struct Cap {
    QByteArray name;
    qreal x, y, w, h;
    qreal x2 = 0, w2 = 0; // a tall Enter's wider top
};

// Keys of `width` after each other on row `y` from `x`, named from `names`.
void row(QList<Cap>& caps, qreal& x, qreal y, std::initializer_list<std::pair<const char*, qreal>> keys)
{
    for (const auto& [name, width] : keys) {
        caps.append({ name, x, y, width, 1 });
        x += width;
    }
}

// `count` keys of a row of letters: AD01, AD02...
void letters(QList<Cap>& caps, qreal& x, qreal y, const char* prefix, int count)
{
    for (int i = 1; i <= count; ++i) {
        caps.append({ QByteArray(prefix) + QByteArray::number(i).rightJustified(2, '0'), x, y, 1, 1 });
        x += 1;
    }
}

// The main block of a keyboard of `shape`, 15 units wide and 5 rows high:
// ISO 105, ANSI 104, JIS 109 and ABNT2 107 keys, as XKB names them
// (evdev keycodes: the ISO key left of Enter and ANSI's backslash are both
// BKSL; JIS's yen is AE13 and its ro AB11, which ABNT2 has too).
QList<Cap> shapeOf(const QString& shape)
{
    QList<Cap> caps;
    const bool ansi = shape == QLatin1String("ansi");
    const bool jis = shape == QLatin1String("jis");
    const bool abnt = shape == QLatin1String("abnt");
    qreal x = 0;
    row(caps, x, 0, { { "TLDE", 1 } });
    letters(caps, x, 0, "AE", 12);
    if (jis) {
        row(caps, x, 0, { { "AE13", 1 }, { "BKSP", 1 } });
    } else {
        row(caps, x, 0, { { "BKSP", 2 } });
    }
    x = 0;
    row(caps, x, 1, { { "TAB", 1.5 } });
    letters(caps, x, 1, "AD", 12);
    if (ansi) {
        row(caps, x, 1, { { "BKSL", 1.5 } });
    } else {
        caps.append({ "RTRN", 13.75, 1, 1.25, 2, 13.5, 1.5 });
    }
    x = 0;
    row(caps, x, 2, { { "CAPS", 1.75 } });
    letters(caps, x, 2, "AC", 11);
    if (ansi) {
        row(caps, x, 2, { { "RTRN", 2.25 } });
    } else {
        row(caps, x, 2, { { "BKSL", 1 } });
    }
    x = 0;
    if (ansi) {
        row(caps, x, 3, { { "LFSH", 2.25 } });
    } else if (jis) {
        row(caps, x, 3, { { "LFSH", 2.25 } });
    } else {
        row(caps, x, 3, { { "LFSH", 1.25 }, { "LSGT", 1 } });
    }
    letters(caps, x, 3, "AB", 10);
    if (jis || abnt) {
        row(caps, x, 3, { { "AB11", 1 }, { "RTSH", 1.75 } });
    } else {
        row(caps, x, 3, { { "RTSH", 2.75 } });
    }
    x = 0;
    if (jis) {
        row(caps, x, 4,
            { { "LCTL", 1.25 }, { "LWIN", 1.25 }, { "LALT", 1.25 }, { "MUHE", 1.25 }, { "SPCE", 4.75 },
                { "HENK", 1.25 }, { "HKTG", 1.25 }, { "RALT", 1.25 }, { "RCTL", 1.5 } });
    } else {
        row(caps, x, 4,
            { { "LCTL", 1.25 }, { "LWIN", 1.25 }, { "LALT", 1.25 }, { "SPCE", 6.25 }, { "RALT", 1.25 },
                { "RWIN", 1.25 }, { "COMP", 1.25 }, { "RCTL", 1.25 } });
    }
    return caps;
}

// What a dead key adds, shown on its cap.
QString deadMark(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_dead_grave: return QStringLiteral("`");
    case XKB_KEY_dead_acute: return QStringLiteral("´");
    case XKB_KEY_dead_circumflex: return QStringLiteral("^");
    case XKB_KEY_dead_tilde: return QStringLiteral("~");
    case XKB_KEY_dead_macron: return QStringLiteral("¯");
    case XKB_KEY_dead_breve: return QStringLiteral("˘");
    case XKB_KEY_dead_abovedot: return QStringLiteral("˙");
    case XKB_KEY_dead_diaeresis: return QStringLiteral("¨");
    case XKB_KEY_dead_abovering: return QStringLiteral("°");
    case XKB_KEY_dead_doubleacute: return QStringLiteral("˝");
    case XKB_KEY_dead_caron: return QStringLiteral("ˇ");
    case XKB_KEY_dead_cedilla: return QStringLiteral("¸");
    case XKB_KEY_dead_ogonek: return QStringLiteral("˛");
    case XKB_KEY_dead_greek: return QStringLiteral("µ");
    default: return {};
    }
}

struct Label {
    QString text;
    bool dead = false;
    bool special = false;
};

// A keysym on a cap: its character, a dead key's mark, or the key's name.
Label labelOf(xkb_keysym_t sym)
{
    if (sym == XKB_KEY_NoSymbol || sym == XKB_KEY_space) {
        return {};
    }
    const QString mark = deadMark(sym);
    if (!mark.isEmpty()) {
        return { mark, true, false };
    }
    const char32_t code = xkb_keysym_to_utf32(sym);
    if (code >= 0x20 && code != 0x7f) {
        return { QString::fromUcs4(&code, 1), false, false };
    }
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R: return { KeyboardPreview::tr("Shift"), false, true };
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R: return { KeyboardPreview::tr("Ctrl"), false, true };
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R: return { KeyboardPreview::tr("Alt"), false, true };
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R: return { KeyboardPreview::tr("Win"), false, true };
    case XKB_KEY_ISO_Level3_Shift: return { KeyboardPreview::tr("AltGr"), false, true };
    case XKB_KEY_Caps_Lock: return { KeyboardPreview::tr("Caps Lock"), false, true };
    case XKB_KEY_Multi_key: return { KeyboardPreview::tr("Compose"), false, true };
    case XKB_KEY_Escape: return { KeyboardPreview::tr("Esc"), false, true };
    case XKB_KEY_BackSpace: return { KeyboardPreview::tr("Backspace"), false, true };
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab: return { KeyboardPreview::tr("Tab"), false, true };
    case XKB_KEY_Return: return { KeyboardPreview::tr("Enter"), false, true };
    case XKB_KEY_Menu: return { KeyboardPreview::tr("Menu"), false, true };
    case XKB_KEY_Num_Lock: return { KeyboardPreview::tr("Num Lock"), false, true };
    default: break;
    }
    char name[64];
    if (xkb_keysym_get_name(sym, name, sizeof(name)) > 0) {
        return { QString::fromLatin1(name), false, true };
    }
    return {};
}

// The compose table of the user's locale, as the apps use; English's when
// the locale has none (C, POSIX).
xkb_compose_table* composeTable(xkb_context* context)
{
    const char* locale = nullptr;
    for (const char* variable : { "LC_ALL", "LC_CTYPE", "LANG" }) {
        const char* value = getenv(variable);
        if (value && *value) {
            locale = value;
            break;
        }
    }
    xkb_compose_table* table = nullptr;
    if (locale && strcmp(locale, "C") != 0 && strcmp(locale, "POSIX") != 0) {
        table = xkb_compose_table_new_from_locale(context, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    }
    if (!table) {
        table = xkb_compose_table_new_from_locale(context, "en_US.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
    }
    return table;
}

bool isEditKey(xkb_keysym_t sym, QString* action)
{
    switch (sym) {
    case XKB_KEY_BackSpace: *action = QStringLiteral("backspace"); return true;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete: *action = QStringLiteral("delete"); return true;
    case XKB_KEY_Left:
    case XKB_KEY_KP_Left: *action = QStringLiteral("left"); return true;
    case XKB_KEY_Right:
    case XKB_KEY_KP_Right: *action = QStringLiteral("right"); return true;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home: *action = QStringLiteral("home"); return true;
    case XKB_KEY_End:
    case XKB_KEY_KP_End: *action = QStringLiteral("end"); return true;
    default: return false;
    }
}

// AltGr: a level 3 that types something of its own on a key of the main
// block, other than the key left of Shift (us gives it | on level 3 though
// few keyboards have it).
bool typesOnLevel3(xkb_keymap* keymap)
{
    for (const Cap& cap : shapeOf(QStringLiteral("iso"))) {
        const xkb_keycode_t key = xkb_keymap_key_by_name(keymap, cap.name.constData());
        if (key == XKB_KEYCODE_INVALID || cap.name == "LSGT" || xkb_keymap_num_levels_for_key(keymap, key, 0) < 3) {
            continue;
        }
        const xkb_keysym_t* third = nullptr;
        const xkb_keysym_t* first = nullptr;
        if (xkb_keymap_key_get_syms_by_level(keymap, key, 0, 2, &third) == 1
            && xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &first) == 1 && third[0] != first[0]) {
            const char32_t code = xkb_keysym_to_utf32(third[0]);
            if ((code > 0x20 && code != 0x7f) || !deadMark(third[0]).isEmpty()) {
                return true;
            }
        }
    }
    return false;
}

// The keymap of a layout with XKB options, or null.
xkb_keymap* compile(xkb_context* context, const QString& layout, const QString& variant, const QString& options)
{
    if (!context) {
        return nullptr;
    }
    const QByteArray layoutName = layout.toUtf8();
    const QByteArray variantName = variant.toUtf8();
    const QByteArray optionNames = options.toUtf8();
    xkb_rule_names names {};
    names.layout = layoutName.constData();
    names.variant = variantName.isEmpty() ? nullptr : variantName.constData();
    // "", not null: null would take XKB_DEFAULT_OPTIONS.
    names.options = optionNames.constData();
    return xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
}

} // namespace

KeyboardPreview::KeyboardPreview(QObject* parent)
    : QObject(parent)
    , m_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS))
{
    if (m_context) {
        m_composeTable = composeTable(m_context);
    }
}

KeyboardPreview::~KeyboardPreview()
{
    xkb_compose_state_unref(m_compose);
    xkb_compose_table_unref(m_composeTable);
    xkb_state_unref(m_state);
    xkb_keymap_unref(m_keymap);
    xkb_context_unref(m_context);
}

QString KeyboardPreview::usualShape(const QString& layout)
{
    if (layout == QLatin1String("jp")) {
        return QStringLiteral("jis");
    }
    if (layout == QLatin1String("br")) {
        return QStringLiteral("abnt");
    }
    // Countries whose keyboards are usually ANSI; elsewhere ISO.
    static const QStringList ansi { QStringLiteral("us"), QStringLiteral("cn"), QStringLiteral("kr"),
        QStringLiteral("tw"), QStringLiteral("th"), QStringLiteral("il"), QStringLiteral("ph"), QStringLiteral("id"),
        QStringLiteral("vn"), QStringLiteral("my") };
    return ansi.contains(layout) ? QStringLiteral("ansi") : QStringLiteral("iso");
}

bool KeyboardPreview::show(const QString& layout, const QString& variant, const QString& options)
{
    xkb_keymap* keymap = compile(m_context, layout, variant, options);
    if (!keymap) {
        return false;
    }
    xkb_keymap_unref(m_keymap);
    m_keymap = keymap;
    m_layout = layout;
    m_variant = variant;
    m_shape = usualShape(layout);
    m_altGr = typesOnLevel3(keymap);
    resetState();
    emit keymapChanged();
    emit keysChanged();
    return true;
}

bool KeyboardPreview::typesOnAltGr(const QString& layout, const QString& variant)
{
    xkb_keymap* keymap = compile(m_context, layout, variant, QString());
    const bool types = keymap && typesOnLevel3(keymap);
    xkb_keymap_unref(keymap);
    return types;
}

void KeyboardPreview::setShape(const QString& shape)
{
    static const QStringList shapes { QStringLiteral("iso"), QStringLiteral("ansi"), QStringLiteral("jis"),
        QStringLiteral("abnt") };
    if (shape == m_shape || !shapes.contains(shape)) {
        return;
    }
    m_shape = shape;
    emit keymapChanged();
    emit keysChanged();
}

void KeyboardPreview::resetState()
{
    xkb_state_unref(m_state);
    m_state = m_keymap ? xkb_state_new(m_keymap) : nullptr;
    xkb_compose_state_unref(m_compose);
    m_compose = m_composeTable ? xkb_compose_state_new(m_composeTable, XKB_COMPOSE_STATE_NO_FLAGS) : nullptr;
    m_pressed.clear();
}

void KeyboardPreview::reset()
{
    resetState();
    emit keysChanged();
}

quint32 KeyboardPreview::keycode(const QString& name) const
{
    if (!m_keymap) {
        return 0;
    }
    const xkb_keycode_t key = xkb_keymap_key_by_name(m_keymap, name.toLatin1().constData());
    return key == XKB_KEYCODE_INVALID ? 0 : key;
}

QVariantList KeyboardPreview::keys() const
{
    QVariantList list;
    if (!m_keymap || !m_state) {
        return list;
    }
    for (const Cap& cap : shapeOf(m_shape)) {
        const xkb_keycode_t key = xkb_keymap_key_by_name(m_keymap, cap.name.constData());
        QVariantList labels;
        QVariantList dead;
        bool special = false;
        const int levels = key == XKB_KEYCODE_INVALID ? 0 : int(xkb_keymap_num_levels_for_key(m_keymap, key, 0));
        for (int level = 0; level < 4; ++level) {
            Label label;
            const xkb_keysym_t* syms = nullptr;
            if (level < levels && xkb_keymap_key_get_syms_by_level(m_keymap, key, 0, level, &syms) == 1) {
                label = labelOf(syms[0]);
            }
            // A name (Shift, Tab) is shown once, on the first level.
            if (level > 0 && special) {
                label = {};
            }
            special = special || label.special;
            labels << label.text;
            dead << label.dead;
        }
        // A letter's capital is shown alone, as on printed keys.
        const QString first = labels.at(0).toString();
        const bool letter = !first.isEmpty() && first != first.toUpper() && labels.at(1).toString() == first.toUpper();
        list.append(QVariantMap {
            { QStringLiteral("name"), QString::fromLatin1(cap.name) },
            { QStringLiteral("x"), cap.x },
            { QStringLiteral("y"), cap.y },
            { QStringLiteral("w"), cap.w },
            { QStringLiteral("h"), cap.h },
            { QStringLiteral("x2"), cap.x2 },
            { QStringLiteral("w2"), cap.w2 },
            { QStringLiteral("labels"), labels },
            { QStringLiteral("dead"), dead },
            { QStringLiteral("letter"), letter },
            { QStringLiteral("special"), special },
            { QStringLiteral("level"), key == XKB_KEYCODE_INVALID ? 0 : int(xkb_state_key_get_level(m_state, key, 0)) },
            { QStringLiteral("pressed"), key != XKB_KEYCODE_INVALID && m_pressed.contains(key) },
        });
    }
    return list;
}

QVariantMap KeyboardPreview::key(quint32 keycode, bool pressed, bool autoRepeat)
{
    QVariantMap result { { QStringLiteral("text"), QString() }, { QStringLiteral("action"), QString() } };
    if (!m_state || keycode < xkb_keymap_min_keycode(m_keymap) || keycode > xkb_keymap_max_keycode(m_keymap)) {
        return result;
    }
    if (!pressed) {
        // An auto-repeat's release is followed by its press: the key is
        // still down.
        if (!autoRepeat && m_pressed.remove(keycode)) {
            xkb_state_update_key(m_state, keycode, XKB_KEY_UP);
            emit keysChanged();
        }
        return result;
    }
    if (autoRepeat && !m_pressed.contains(keycode)) {
        return result; // pressed before the field had the focus
    }
    // The keysym before the key's own press changes the state.
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_state, keycode);
    const auto active = [this](const char* name) {
        return xkb_state_mod_name_is_active(m_state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    QString action;
    if (isEditKey(sym, &action)) {
        result[QStringLiteral("action")] = action;
    } else if (active(XKB_MOD_NAME_ALT) || active(XKB_MOD_NAME_LOGO)) {
        // A shortcut (Ctrl+Alt+A too), neither typing nor editing.
    } else if (active(XKB_MOD_NAME_CTRL)) {
        switch (xkb_keysym_to_lower(sym)) {
        case XKB_KEY_a: result[QStringLiteral("action")] = QStringLiteral("select-all"); break;
        case XKB_KEY_c: result[QStringLiteral("action")] = QStringLiteral("copy"); break;
        case XKB_KEY_x: result[QStringLiteral("action")] = QStringLiteral("cut"); break;
        case XKB_KEY_v: result[QStringLiteral("action")] = QStringLiteral("paste"); break;
        default: break; // a shortcut, not typing
        }
    } else {
        QString text;
        const auto status = m_compose && xkb_compose_state_feed(m_compose, sym) == XKB_COMPOSE_FEED_ACCEPTED
            ? xkb_compose_state_get_status(m_compose)
            : XKB_COMPOSE_NOTHING;
        if (status == XKB_COMPOSE_COMPOSED) {
            std::array<char, 64> buffer {};
            xkb_compose_state_get_utf8(m_compose, buffer.data(), buffer.size());
            text = QString::fromUtf8(buffer.data());
            xkb_compose_state_reset(m_compose);
        } else if (status == XKB_COMPOSE_CANCELLED) {
            xkb_compose_state_reset(m_compose);
        } else if (status == XKB_COMPOSE_NOTHING) {
            std::array<char, 64> buffer {};
            xkb_state_key_get_utf8(m_state, keycode, buffer.data(), buffer.size());
            text = QString::fromUtf8(buffer.data());
        }
        // Enter, Tab, Esc and the like type nothing here.
        if (text.size() == 1 && (text.at(0).unicode() < 0x20 || text.at(0).unicode() == 0x7f)) {
            text.clear();
        }
        result[QStringLiteral("text")] = text;
    }
    if (!autoRepeat) {
        m_pressed.insert(keycode);
        xkb_state_update_key(m_state, keycode, XKB_KEY_DOWN);
        emit keysChanged();
    }
    return result;
}

QString KeyboardPreview::clipboardText() const
{
    const QClipboard* clipboard = QGuiApplication::clipboard();
    return clipboard ? clipboard->text() : QString();
}
