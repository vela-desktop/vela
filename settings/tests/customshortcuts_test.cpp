// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings > Keyboard > Custom shortcuts (settings/src/customshortcuts.h,
// docs/keyboard.md §9): recording with the compositor's matcher, what is
// refused or warned about, the labels, and the file written atomically
// with the user's other lines kept.

#include "customshortcuts.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

namespace {

QVariantList italian()
{
    return { QVariantMap {
        { QStringLiteral("layout"), QStringLiteral("it") }, { QStringLiteral("variant"), QString() } } };
}

QVariantList apps()
{
    return {
        QVariantMap { { QStringLiteral("id"), QStringLiteral("org.kde.kcalc.desktop") },
            { QStringLiteral("name"), QStringLiteral("KCalc") }, { QStringLiteral("icon"), QStringLiteral("kcalc") },
            { QStringLiteral("keys"), QStringLiteral("Calculator") } },
        QVariantMap { { QStringLiteral("id"), QStringLiteral("org.kde.konsole.desktop") },
            { QStringLiteral("name"), QStringLiteral("Konsole") },
            { QStringLiteral("icon"), QStringLiteral("utilities-terminal") },
            { QStringLiteral("keys"), QStringLiteral("Terminal") } },
    };
}

// Qt gives the XKB key code as the native scan code.
int scan(int evdev)
{
    return evdev + 8;
}

} // namespace

class CustomShortcutsTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_config;
    int m_written = 0;
    CustomShortcuts::Modes m_modes;

    std::unique_ptr<CustomShortcuts> make()
    {
        return std::make_unique<CustomShortcuts>(apps, italian, [this] { return m_modes; }, [this] { ++m_written; });
    }
    QByteArray file() const
    {
        QFile in(CustomShortcuts::path());
        return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    }

private slots:
    void init()
    {
        qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
        // Plain keymaps, whatever the developer's XKB_DEFAULT_* say.
        for (const char* name : { "XKB_DEFAULT_RULES", "XKB_DEFAULT_MODEL", "XKB_DEFAULT_LAYOUT", "XKB_DEFAULT_VARIANT",
                 "XKB_DEFAULT_OPTIONS" }) {
            qunsetenv(name);
        }
        QFile::remove(CustomShortcuts::path());
        m_written = 0;
        m_modes = {};
    }

    void recordsWithTheCompositorsRules()
    {
        auto shortcuts = make();
        // A modifier alone: still waiting.
        QCOMPARE(shortcuts->press(scan(KEY_LEFTCTRL), Qt::ControlModifier).value("state").toString(), "modifier");
        // Ctrl+Alt+T: allowed, warned about (apps use it).
        QVariantMap r = shortcuts->press(scan(KEY_T), Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(r.value("state").toString(), "ok");
        QCOMPARE(r.value("combo").toString(), "Ctrl+Alt+t");
        QCOMPARE(r.value("label").toString(), "Ctrl+Alt+T");
        QVERIFY(r.value("warning").toBool());
        // Win+Shift+K: allowed, no warning.
        r = shortcuts->press(scan(KEY_K), Qt::MetaModifier | Qt::ShiftModifier);
        QCOMPARE(r.value("state").toString(), "ok");
        QCOMPARE(r.value("combo").toString(), "Shift+Super+k");
        QCOMPARE(r.value("label").toString(), "Win+Shift+K");
        QVERIFY(!r.value("warning").toBool());
        // Win+Shift+E is free now (exact modifiers), Win+E is Vela's.
        QCOMPARE(shortcuts->press(scan(KEY_E), Qt::MetaModifier | Qt::ShiftModifier).value("state").toString(), "ok");
        r = shortcuts->press(scan(KEY_E), Qt::MetaModifier);
        QCOMPARE(r.value("state").toString(), "refused");
        QVERIFY(!r.value("warning").toBool());
        // Reserved: Win+L, Alt+Shift+Esc.
        QCOMPARE(shortcuts->press(scan(KEY_L), Qt::MetaModifier).value("reason").toString(),
            QStringLiteral("This combination is reserved for Vela."));
        QCOMPARE(
            shortcuts->press(scan(KEY_ESC), Qt::AltModifier | Qt::ShiftModifier).value("state").toString(), "refused");
        // A key alone, or with Shift, types.
        QCOMPARE(shortcuts->press(scan(KEY_A), Qt::NoModifier).value("state").toString(), "refused");
        QCOMPARE(shortcuts->press(scan(KEY_A), Qt::ShiftModifier).value("state").toString(), "refused");
        QCOMPARE(shortcuts->press(scan(KEY_F5), Qt::NoModifier).value("state").toString(), "refused");
        // F13–F24 and the special keys Vela doesn't use, alone.
        r = shortcuts->press(scan(KEY_F13), Qt::NoModifier);
        QCOMPARE(r.value("state").toString(), "ok");
        QCOMPARE(r.value("combo").toString(), "FK13");
        QCOMPARE(shortcuts->press(scan(KEY_CALC), Qt::NoModifier).value("state").toString(), "ok");
        // ...but not the volume.
        QCOMPARE(shortcuts->press(scan(KEY_VOLUMEUP), Qt::NoModifier).value("state").toString(), "refused");
        // The digit row by position, named by its digit on Italian too.
        r = shortcuts->press(scan(KEY_1), Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(r.value("combo").toString(), "Ctrl+Alt+AE01");
        QCOMPARE(r.value("label").toString(), "Ctrl+Alt+1");
        // Italian's own letters: ò is a symbol.
        r = shortcuts->press(scan(KEY_SEMICOLON), Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(r.value("combo").toString(), "Ctrl+Alt+ograve");
        QCOMPARE(r.value("label").toString(), "Ctrl+Alt+Ò");
    }

    void modesSayWhatIsTaken()
    {
        auto shortcuts = make();
        // Win+Ctrl+C: free until the color-filter shortcut is on.
        QCOMPARE(shortcuts->press(scan(KEY_C), Qt::MetaModifier | Qt::ControlModifier).value("state").toString(), "ok");
        m_modes.colorFilterShortcut = true;
        QCOMPARE(
            shortcuts->press(scan(KEY_C), Qt::MetaModifier | Qt::ControlModifier).value("state").toString(), "refused");
        // Win+Alt+1: the jump list with super-number-keys=apps, free with
        // desktops or off.
        QCOMPARE(
            shortcuts->press(scan(KEY_1), Qt::MetaModifier | Qt::AltModifier).value("state").toString(), "refused");
        m_modes.superNumberKeys = QStringLiteral("desktops");
        QCOMPARE(shortcuts->press(scan(KEY_1), Qt::MetaModifier | Qt::AltModifier).value("state").toString(), "ok");
        QCOMPARE(shortcuts->press(scan(KEY_1), Qt::MetaModifier).value("state").toString(), "refused");
        m_modes.superNumberKeys = QStringLiteral("off");
        QCOMPARE(shortcuts->press(scan(KEY_1), Qt::MetaModifier).value("state").toString(), "ok");
    }

    void targets()
    {
        auto shortcuts = make();
        QVERIFY(shortcuts->targetProblem("app", "org.kde.kcalc.desktop").isEmpty());
        QVERIFY(!shortcuts->targetProblem("app", "").isEmpty());
        QVERIFY(!shortcuts->targetProblem("app", "../evil.desktop").isEmpty());
        QVERIFY(shortcuts->targetProblem("command", "notify-send 'Hello there'").isEmpty());
        QVERIFY(!shortcuts->targetProblem("command", "echo a\nrm -rf b").isEmpty());
        QVERIFY(!shortcuts->targetProblem("command", "a\tb").isEmpty());
        QVERIFY(shortcuts->targetProblem("open", "https://example.org/").isEmpty());
        QVERIFY(shortcuts->targetProblem("open", "/home/user/notes.txt").isEmpty());
        QVERIFY(!shortcuts->targetProblem("open", "notes.txt").isEmpty());
        QVERIFY(!shortcuts->targetProblem("command", QString(5000, u'x')).isEmpty());
        // The apps to choose from, by name or keyword.
        QCOMPARE(shortcuts->apps("calcul").size(), 1);
        QCOMPARE(shortcuts->apps("").size(), 2);
    }

    void savesReplacesAndRemoves()
    {
        // The user's own lines and comments stay.
        QVERIFY(QDir().mkpath(m_config.path() + "/vela"));
        {
            QFile out(CustomShortcuts::path());
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write("# mine\nnot a shortcut\nalt+ctrl+T=command xterm\n");
        }
        auto shortcuts = make();
        QCOMPARE(shortcuts->items().size(), 1);
        QVariantMap item = shortcuts->items().at(0).toMap();
        QCOMPARE(item.value("combo").toString(), "Ctrl+Alt+t");
        QCOMPARE(item.value("label").toString(), "Ctrl+Alt+T");
        QCOMPARE(item.value("target").toString(), "xterm");
        // Add an app: its name and icon.
        QVERIFY(shortcuts->save(-1, "Shift+Super+k", "app", "org.kde.kcalc.desktop"));
        QCOMPARE(m_written, 1);
        QCOMPARE(file(),
            QByteArray("# mine\nnot a shortcut\nCtrl+Alt+t=command xterm\n"
                       "Shift+Super+k=app org.kde.kcalc.desktop\n"));
        item = shortcuts->items().at(1).toMap();
        QCOMPARE(item.value("name").toString(), "KCalc");
        QCOMPARE(item.value("icon").toString(), "kcalc");
        // Edit the first: another combination and kind.
        QVERIFY(shortcuts->save(0, "Ctrl+Alt+AE01", "open", "https://example.org/"));
        QCOMPARE(file(),
            QByteArray("# mine\nnot a shortcut\nCtrl+Alt+AE01=open https://example.org/\n"
                       "Shift+Super+k=app org.kde.kcalc.desktop\n"));
        // A new one with the second's combination replaces it.
        QVERIFY(shortcuts->save(-1, "Shift+Super+k", "command", "kcalc"));
        QCOMPARE(shortcuts->items().size(), 2);
        QCOMPARE(file(),
            QByteArray("# mine\nnot a shortcut\nCtrl+Alt+AE01=open https://example.org/\n"
                       "Shift+Super+k=command kcalc\n"));
        // Editing one onto another's combination: one line left.
        QVERIFY(shortcuts->save(0, "Shift+Super+k", "open", "/tmp/a"));
        QCOMPARE(file(), QByteArray("# mine\nnot a shortcut\nShift+Super+k=open /tmp/a\n"));
        // Refused: a bad combination, kind or target.
        QVERIFY(!shortcuts->save(-1, "Hyper+x", "command", "x"));
        QVERIFY(!shortcuts->save(-1, "Super+x", "script", "x"));
        QVERIFY(!shortcuts->save(-1, "Super+j", "command", "a\nb"));
        QCOMPARE(m_written, 4);
        QVERIFY(shortcuts->remove(0));
        QCOMPARE(file(), QByteArray("# mine\nnot a shortcut\n"));
        QVERIFY(!shortcuts->remove(0));
        QVERIFY(shortcuts->items().isEmpty());
    }

    void recordsTheActiveLayout()
    {
        auto shortcuts = make();
        xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        const xkb_rule_names names { "evdev", "pc105", "us,fr", nullptr, nullptr };
        xkb_keymap* map = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        QVERIFY(map);
        xkb_state* state = xkb_state_new(map);
        const auto ctrl = 1u << xkb_keymap_mod_get_index(map, XKB_MOD_NAME_CTRL);
        xkb_state_update_mask(state, ctrl, 0, 0, 0, 0, 1);
        QCOMPARE(shortcuts->pressWithState(scan(KEY_Q), state).value("combo").toString(), "Ctrl+a");
        xkb_state_unref(state);
        xkb_keymap_unref(map);
        xkb_context_unref(context);
    }

    void detectsAnExternalEditBeforeSavingOrRemoving()
    {
        auto first = make();
        QVERIFY(first->save(-1, "Ctrl+Alt+q", "command", "echo one"));
        auto second = make();
        QVERIFY(first->save(-1, "Ctrl+Alt+w", "command", "echo two"));
        QVERIFY(!second->save(0, "Ctrl+Alt+q", "command", "echo three"));
        QCOMPARE(second->items().size(), 2);
        QVERIFY(file().contains("echo two"));
        QVERIFY(second->save(0, "Ctrl+Alt+q", "command", "echo three"));
        QVERIFY(!first->remove(0));
        QVERIFY(file().contains("echo three"));
    }

    void newFileHasItsHeader()
    {
        auto shortcuts = make();
        QVERIFY(shortcuts->save(-1, "Super+FK01", "open", "https://docs.example.org/"));
        QCOMPARE(file(),
            QByteArray("# Vela's custom shortcuts (Settings > Keyboard > Custom shortcuts)\n"
                       "Super+FK01=open https://docs.example.org/\n"));
        QCOMPARE(shortcuts->items().at(0).toMap().value("label").toString(), "Win+F1");
    }
};

QTEST_GUILESS_MAIN(CustomShortcutsTest)
#include "customshortcuts_test.moc"
