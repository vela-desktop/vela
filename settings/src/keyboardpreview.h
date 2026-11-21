// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QSet>
#include <QVariantList>

struct xkb_compose_state;
struct xkb_compose_table;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

// A layout tried before it is added (docs/keyboard.md §5.4): its keymap,
// compiled here, drawn as a keyboard of one of four shapes, and typed with
// through its own XKB state, so the layout the desktop uses never changes.
class KeyboardPreview : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString layout READ layout NOTIFY keymapChanged)
    Q_PROPERTY(QString variant READ variant NOTIFY keymapChanged)
    // "iso", "ansi", "jis" or "abnt": the keyboard drawn.
    Q_PROPERTY(QString shape READ shape WRITE setShape NOTIFY keymapChanged)
    // The keys, in units of a key's width: [{name, x, y, w, h, x2, w2,
    // labels: [level 1, 2, 3, 4], dead: [4 bools], level, pressed,
    // special}]. A tall Enter has h 2 and its top row's wider part in x2,
    // w2; `level` is the one the key would type now (0-3); `special`: a key
    // with a name (Shift, Tab...) rather than characters.
    Q_PROPERTY(QVariantList keys READ keys NOTIFY keysChanged)
    // The keyboard's size in units.
    Q_PROPERTY(qreal columns READ columns NOTIFY keymapChanged)
    Q_PROPERTY(qreal rows READ rows NOTIFY keymapChanged)
    // Whether the layout types anything on AltGr (level 3).
    Q_PROPERTY(bool altGr READ altGr NOTIFY keymapChanged)

public:
    explicit KeyboardPreview(QObject* parent = nullptr);
    ~KeyboardPreview() override;

    QString layout() const { return m_layout; }
    QString variant() const { return m_variant; }
    QString shape() const { return m_shape; }
    void setShape(const QString& shape);
    QVariantList keys() const;
    qreal columns() const { return 15; }
    qreal rows() const { return 5; }
    bool altGr() const { return m_altGr; }

    // Compiles the layout's keymap with XKB `options` and resets the state
    // and the shape to the layout's usual one. false if it doesn't compile.
    Q_INVOKABLE bool show(const QString& layout, const QString& variant, const QString& options = QString());
    // A key event of the try field: the XKB key code (on Wayland,
    // QKeyEvent::nativeScanCode()), pressed or released, auto-repeated.
    // {text, action}: the text to insert, or an action on the field
    // ("backspace", "delete", "left", "right", "home", "end",
    // "select-all", "copy", "cut", "paste"); both empty for nothing.
    Q_INVOKABLE QVariantMap key(quint32 keycode, bool pressed, bool autoRepeat);
    // Whether the layout types characters on AltGr (Settings warns before
    // Compose takes Right Alt).
    Q_INVOKABLE bool typesOnAltGr(const QString& layout, const QString& variant);
    // Lets go of every key: the field lost the focus.
    Q_INVOKABLE void reset();
    // The clipboard's text, for a paste into the read-only field.
    Q_INVOKABLE QString clipboardText() const;

    // The shape a layout's keyboards usually have.
    static QString usualShape(const QString& layout);
    // The key code of an XKB key name ("AC01"), 0 if none.
    quint32 keycode(const QString& name) const;

signals:
    void keymapChanged();
    void keysChanged();

private:
    void resetState();

    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
    xkb_state* m_state = nullptr;
    xkb_compose_table* m_composeTable = nullptr;
    xkb_compose_state* m_compose = nullptr;
    QString m_layout;
    QString m_variant;
    QString m_shape = QStringLiteral("iso");
    bool m_altGr = false;
    QSet<quint32> m_pressed;
};
