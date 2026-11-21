// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Settings > Keyboard > Custom shortcuts (docs/keyboard.md §9): the user's
// shortcuts in $XDG_CONFIG_HOME/vela/shortcuts.conf, read and written with
// common/shortcuts/custom.h, and the recording of a combination with the
// matcher and Vela's table the compositor uses (common/shortcuts), so what
// is accepted here is what the compositor runs.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <optional>

class QWindow;
struct wl_registry;
struct zwp_keyboard_shortcuts_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibitor_v1;
struct wl_keyboard;
struct xkb_state;

class CustomShortcuts : public QObject {
    Q_OBJECT
    // [{combo, label, kind, target, name, icon}], in the file's order.
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)

public:
    // The modes Vela's table depends on, as Settings knows them: the
    // color-filter shortcut and Win+number (Preferences).
    struct Modes {
        bool colorFilterShortcut = false;
        QString superNumberKeys = QStringLiteral("apps");
    };
    // The installed apps: [{id, name, icon, keys (more words to search)}].
    using AppsSource = std::function<QVariantList()>;
    // The layouts in use, first first: {layout, variant} maps.
    using LayoutsSource = std::function<QVariantList()>;
    using ModesSource = std::function<Modes()>;
    // The file was written: the compositor rereads it.
    using Written = std::function<void()>;

    CustomShortcuts(
        AppsSource apps, LayoutsSource layouts, ModesSource modes, Written written, QObject* parent = nullptr);
    ~CustomShortcuts() override;

    QVariantList items() const { return m_items; }
    bool recording() const { return m_inhibitor != nullptr; }

    Q_INVOKABLE void reload();
    void refreshItems();

    // While recording, Win and Vela's combinations reach `window` instead of
    // acting (zwp_keyboard_shortcuts_inhibitor_v1).
    Q_INVOKABLE void startRecording(QWindow* window);
    Q_INVOKABLE void stopRecording();

    // A key pressed while recording, as Qt gives it (nativeScanCode is the
    // XKB key code on Wayland; Qt's modifiers). {state: "modifier" (a
    // modifier alone: keep waiting), "ok" or "refused", combo, label,
    // reason, warning (no Win: apps use it), existing (the index of a
    // custom shortcut with it, or -1)}.
    Q_INVOKABLE QVariantMap press(int scanCode, int modifiers) const;
    QVariantMap pressWithState(int scanCode, xkb_state* state) const;

    // The installed apps to choose from: [{id, name, icon}], by name.
    Q_INVOKABLE QVariantList apps(const QString& filter) const;
    // Why `target` can't be used for `kind` ("" if it can).
    Q_INVOKABLE QString targetProblem(const QString& kind, const QString& target) const;
    // The combination as Settings shows it ("Win+Shift+K"), from its
    // normalized text.
    Q_INVOKABLE QString label(const QString& combo) const;

    // Index -1 adds; another replaces that shortcut. A shortcut that had
    // the same combination is replaced. Written atomically, then the
    // compositor rereads it. false if it couldn't be written.
    Q_INVOKABLE bool save(int index, const QString& combo, const QString& kind, const QString& target);
    Q_INVOKABLE bool remove(int index);

    // The file, for the tests.
    static QString path();
    // The label of a combination from its text, with the layouts' symbols.
    static QString labelFor(const QString& combo, const QVariantList& layouts);

signals:
    void changed();
    void recordingChanged();

private:
    struct Line {
        QString text; // as in the file, when it doesn't parse
        QString combo; // normalized; empty for comments and unknown lines
        QString kind;
        QString target;
    };
    bool write(const QList<Line>& lines);
    void bindManager();

    AppsSource m_apps;
    LayoutsSource m_layouts;
    ModesSource m_modes;
    Written m_written;
    std::optional<QByteArray> m_snapshot;
    QList<Line> m_lines;
    QVariantList m_items;
    wl_keyboard* m_keyboard = nullptr;
    xkb_state* m_state = nullptr;
    wl_registry* m_registry = nullptr;
    zwp_keyboard_shortcuts_inhibit_manager_v1* m_manager = nullptr;
    zwp_keyboard_shortcuts_inhibitor_v1* m_inhibitor = nullptr;
};
