// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Trying a layout before adding it (src/keyboardpreview.h, docs/keyboard.md
// §5.4): the keymap compiled from the system's XKB data, the keys drawn for
// each shape, and the try field's typing through the candidate's own state.

#include "keyboardpreview.h"

#include <QSignalSpy>
#include <QTest>

namespace {

QVariantMap keyNamed(const KeyboardPreview& preview, const QString& name)
{
    for (const QVariant& value : preview.keys()) {
        const QVariantMap key = value.toMap();
        if (key.value(QStringLiteral("name")) == name) {
            return key;
        }
    }
    return {};
}

QStringList labels(const KeyboardPreview& preview, const QString& name)
{
    QStringList list;
    for (const QVariant& label : keyNamed(preview, name).value(QStringLiteral("labels")).toList()) {
        list << label.toString();
    }
    return list;
}

// Presses and releases keys by name: "+LFSH" holds, "-LFSH" releases, a
// bare name is pressed and released. The text typed, actions as [action].
QString type(KeyboardPreview& preview, const QStringList& keys)
{
    QString typed;
    for (const QString& key : keys) {
        const bool hold = key.startsWith(u'+');
        const bool release = key.startsWith(u'-');
        const quint32 code = preview.keycode(hold || release ? key.mid(1) : key);
        if (!code) {
            return QStringLiteral("<no key %1>").arg(key);
        }
        if (!release) {
            const QVariantMap result = preview.key(code, true, false);
            typed += result.value(QStringLiteral("text")).toString();
            const QString action = result.value(QStringLiteral("action")).toString();
            if (!action.isEmpty()) {
                typed += u'[' + action + u']';
            }
        }
        if (!hold) {
            preview.key(code, false, false);
        }
    }
    return typed;
}

} // namespace

class KeyboardPreviewTest : public QObject {
    Q_OBJECT

private slots:
    void italianIsDrawnOnAnIsoKeyboard()
    {
        KeyboardPreview preview;
        QSignalSpy changed(&preview, &KeyboardPreview::keymapChanged);
        QVERIFY(preview.show(QStringLiteral("it"), QString()));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(preview.shape(), QStringLiteral("iso"));
        QVERIFY(preview.altGr());
        // ò, ç on Shift, @ on AltGr.
        QCOMPARE(labels(preview, QStringLiteral("AC10")).mid(0, 3),
            (QStringList { QStringLiteral("ò"), QStringLiteral("ç"), QStringLiteral("@") }));
        // A letter's capital alone; names for the modifiers, once.
        QVERIFY(keyNamed(preview, QStringLiteral("AC01")).value(QStringLiteral("letter")).toBool());
        QCOMPARE(labels(preview, QStringLiteral("LFSH")),
            (QStringList { QStringLiteral("Shift"), QString(), QString(), QString() }));
        QVERIFY(keyNamed(preview, QStringLiteral("LFSH")).value(QStringLiteral("special")).toBool());
        QCOMPARE(labels(preview, QStringLiteral("RALT")).first(), QStringLiteral("AltGr"));
        // ISO: the key left of the short left Shift and a tall Enter.
        QVERIFY(!keyNamed(preview, QStringLiteral("LSGT")).isEmpty());
        const QVariantMap enter = keyNamed(preview, QStringLiteral("RTRN"));
        QCOMPARE(enter.value(QStringLiteral("h")).toReal(), 2.0);
        QCOMPARE(enter.value(QStringLiteral("w2")).toReal(), 1.5);
        // Every row is 15 units wide.
        for (int y = 0; y < 5; ++y) {
            qreal width = 0;
            for (const QVariant& value : preview.keys()) {
                const QVariantMap key = value.toMap();
                const bool tallEnterRow = key.value(QStringLiteral("name")) == QLatin1String("RTRN") && y == 2;
                if (key.value(QStringLiteral("y")).toInt() == y || tallEnterRow) {
                    width += key.value(QStringLiteral("w")).toReal();
                }
            }
            if (y == 1) {
                width += 0.25; // the tall Enter's wider top
            }
            QCOMPARE(width, 15.0);
        }
    }

    void eachLayoutHasItsUsualShape()
    {
        KeyboardPreview preview;
        QVERIFY(preview.show(QStringLiteral("us"), QString()));
        QCOMPARE(preview.shape(), QStringLiteral("ansi"));
        QVERIFY(keyNamed(preview, QStringLiteral("LSGT")).isEmpty());
        QCOMPARE(keyNamed(preview, QStringLiteral("RTRN")).value(QStringLiteral("h")).toReal(), 1.0);
        QVERIFY(!preview.altGr());
        QVERIFY(preview.show(QStringLiteral("jp"), QString()));
        QCOMPARE(preview.shape(), QStringLiteral("jis"));
        QVERIFY(!keyNamed(preview, QStringLiteral("AE13")).isEmpty());
        QVERIFY(!keyNamed(preview, QStringLiteral("AB11")).isEmpty());
        QVERIFY(preview.show(QStringLiteral("br"), QString()));
        QCOMPARE(preview.shape(), QStringLiteral("abnt"));
        QVERIFY(!keyNamed(preview, QStringLiteral("AB11")).isEmpty());
        QVERIFY(!keyNamed(preview, QStringLiteral("LSGT")).isEmpty());
        // The shape can be switched; an unknown one is ignored.
        preview.setShape(QStringLiteral("ansi"));
        QCOMPARE(preview.shape(), QStringLiteral("ansi"));
        preview.setShape(QStringLiteral("round"));
        QCOMPARE(preview.shape(), QStringLiteral("ansi"));
        QVERIFY(!preview.show(QStringLiteral("no-such-layout"), QString()));
        QCOMPARE(preview.layout(), QStringLiteral("br"));
    }

    void typingGoesThroughTheCandidateLayout()
    {
        KeyboardPreview preview;
        QVERIFY(preview.show(QStringLiteral("it"), QString()));
        QCOMPARE(type(preview, { QStringLiteral("AC10"), QStringLiteral("AC11") }), QStringLiteral("òà"));
        QCOMPARE(
            type(preview,
                { QStringLiteral("+LFSH"), QStringLiteral("AD03"), QStringLiteral("-LFSH"), QStringLiteral("AD03") }),
            QStringLiteral("Ee"));
        QCOMPARE(type(preview, { QStringLiteral("+RALT"), QStringLiteral("AD03"), QStringLiteral("-RALT") }),
            QStringLiteral("€"));
        // The drawing follows the held keys.
        preview.key(preview.keycode(QStringLiteral("LFSH")), true, false);
        QVERIFY(keyNamed(preview, QStringLiteral("LFSH")).value(QStringLiteral("pressed")).toBool());
        QCOMPARE(keyNamed(preview, QStringLiteral("AC10")).value(QStringLiteral("level")).toInt(), 1);
        preview.key(preview.keycode(QStringLiteral("LFSH")), false, false);
        QCOMPARE(keyNamed(preview, QStringLiteral("AC10")).value(QStringLiteral("level")).toInt(), 0);
    }

    void deadKeysAndComposeCombine()
    {
        KeyboardPreview preview;
        QVERIFY(preview.show(QStringLiteral("us"), QStringLiteral("intl")));
        const QString acute = type(preview, { QStringLiteral("AC11"), QStringLiteral("AD03") });
        if (acute == QLatin1String("e")) {
            QSKIP("No compose table for the locale (X11's locale data is not installed)");
        }
        QCOMPARE(acute, QStringLiteral("é"));
        QVERIFY(keyNamed(preview, QStringLiteral("AC11")).value(QStringLiteral("dead")).toList().first().toBool());
        QVERIFY(preview.show(QStringLiteral("us"), QString(), QStringLiteral("compose:ralt")));
        QCOMPARE(labels(preview, QStringLiteral("RALT")).first(), QStringLiteral("Compose"));
        // Compose, o, c.
        QCOMPARE(type(preview, { QStringLiteral("RALT"), QStringLiteral("AD09"), QStringLiteral("AB03") }),
            QStringLiteral("©"));
    }

    void autoRepeatTypesAgainWithoutStickingKeys()
    {
        KeyboardPreview preview;
        QVERIFY(preview.show(QStringLiteral("us"), QString()));
        const quint32 shift = preview.keycode(QStringLiteral("LFSH"));
        const quint32 a = preview.keycode(QStringLiteral("AC01"));
        preview.key(shift, true, false);
        QCOMPARE(preview.key(a, true, false).value(QStringLiteral("text")).toString(), QStringLiteral("A"));
        // Qt's auto-repeat: a release and a press, both marked.
        preview.key(a, false, true);
        QCOMPARE(preview.key(a, true, true).value(QStringLiteral("text")).toString(), QStringLiteral("A"));
        preview.key(shift, true, true);
        preview.key(a, false, false);
        preview.key(shift, false, false);
        QCOMPARE(preview.key(a, true, false).value(QStringLiteral("text")).toString(), QStringLiteral("a"));
        preview.key(a, false, false);
        // A repeat of a key pressed elsewhere types nothing.
        QCOMPARE(preview.key(a, true, true).value(QStringLiteral("text")).toString(), QString());
        // Leaving the field lets go of what is held.
        preview.key(shift, true, false);
        preview.reset();
        QVERIFY(!keyNamed(preview, QStringLiteral("LFSH")).value(QStringLiteral("pressed")).toBool());
        QCOMPARE(type(preview, { QStringLiteral("AC01") }), QStringLiteral("a"));
    }

    void editingKeysAreActionsAndShortcutsTypeNothing()
    {
        KeyboardPreview preview;
        QVERIFY(preview.show(QStringLiteral("fr"), QString()));
        QCOMPARE(type(preview, { QStringLiteral("BKSP"), QStringLiteral("RTRN"), QStringLiteral("TAB") }),
            QStringLiteral("[backspace]"));
        // Ctrl+A by the candidate's letters: AZERTY's A is QWERTY's Q.
        QCOMPARE(type(preview,
                     { QStringLiteral("+LCTL"), QStringLiteral("AD01"), QStringLiteral("AB04"), QStringLiteral("AD05"),
                         QStringLiteral("-LCTL") }),
            QStringLiteral("[select-all][paste]"));
        QCOMPARE(
            type(preview, { QStringLiteral("+LALT"), QStringLiteral("AD01"), QStringLiteral("-LALT") }), QString());
        // Ctrl with Alt or Win is a shortcut, not an edit.
        QCOMPARE(type(preview,
                     { QStringLiteral("+LCTL"), QStringLiteral("+LALT"), QStringLiteral("AD01"),
                         QStringLiteral("-LALT"), QStringLiteral("+LWIN"), QStringLiteral("AD01"),
                         QStringLiteral("-LWIN"), QStringLiteral("-LCTL") }),
            QString());
    }

    void optionsChangeTheKeys()
    {
        KeyboardPreview preview;
        QVERIFY(preview.typesOnAltGr(QStringLiteral("it"), QString()));
        QVERIFY(preview.typesOnAltGr(QStringLiteral("us"), QStringLiteral("intl")));
        QVERIFY(!preview.typesOnAltGr(QStringLiteral("us"), QString()));
        QVERIFY(!preview.typesOnAltGr(QStringLiteral("no-such-layout"), QString()));
        QVERIFY(preview.show(QStringLiteral("us"), QString(), QStringLiteral("ctrl:swap_lalt_lctl")));
        QCOMPARE(labels(preview, QStringLiteral("LALT")).first(), QStringLiteral("Ctrl"));
        QCOMPARE(labels(preview, QStringLiteral("LCTL")).first(), QStringLiteral("Alt"));
        QVERIFY(preview.show(QStringLiteral("us"), QString(), QStringLiteral("caps:escape")));
        QCOMPARE(labels(preview, QStringLiteral("CAPS")).first(), QStringLiteral("Esc"));
        // No options means none, whatever XKB_DEFAULT_OPTIONS says.
        qputenv("XKB_DEFAULT_OPTIONS", "ctrl:nocaps");
        QVERIFY(preview.show(QStringLiteral("us"), QString(), QString()));
        qunsetenv("XKB_DEFAULT_OPTIONS");
        QCOMPARE(labels(preview, QStringLiteral("CAPS")).first(), QStringLiteral("Caps Lock"));
    }
};

QTEST_MAIN(KeyboardPreviewTest)
#include "keyboardpreview_test.moc"
