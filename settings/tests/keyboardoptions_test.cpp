// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Keyboard > Typing's keys over the XKB options (src/keyboardoptions.h,
// docs/keyboard.md §6).

#include "keyboardoptions.h"

#include <QTest>

using namespace vela::keyboardoptions;

class KeyboardOptionsTest : public QObject {
    Q_OBJECT

private slots:
    void eachControlReadsItsOptions()
    {
        const QStringList options = split(QStringLiteral("terminate:ctrl_alt_bksp, caps:escape,compose:ralt,"
                                                         "altwin:swap_lalt_lwin,numpad:mac,grp:alt_shift_toggle"));
        QCOMPARE(value(options, Control::Caps), QStringLiteral("escape"));
        QCOMPARE(value(options, Control::Compose), QStringLiteral("ralt"));
        QCOMPARE(value(options, Control::Corner), QStringLiteral("alt-win"));
        QCOMPARE(value(options, Control::Numpad), QStringLiteral("digits"));
        // None: the defaults; caps:capslock is the default too.
        QCOMPARE(value({}, Control::Caps), QStringLiteral("capslock"));
        QCOMPARE(value({ QStringLiteral("caps:capslock") }, Control::Caps), QStringLiteral("capslock"));
        QCOMPARE(value({}, Control::Compose), QStringLiteral("none"));
        QCOMPARE(value({}, Control::Corner), QStringLiteral("standard"));
        QCOMPARE(value({}, Control::Numpad), QStringLiteral("default"));
        QCOMPARE(value({ QStringLiteral("ctrl:nocaps") }, Control::Caps), QStringLiteral("ctrl"));
        QCOMPARE(value({ QStringLiteral("ctrl:swap_lalt_lctl") }, Control::Corner), QStringLiteral("ctrl-alt"));
        // ctrl:nocaps is Caps Lock's, not the bottom-left keys'.
        QCOMPARE(value({ QStringLiteral("ctrl:nocaps") }, Control::Corner), QStringLiteral("standard"));
    }

    void optionsTheControlsDontOfferAreCustom()
    {
        QCOMPARE(value({ QStringLiteral("ctrl:swapcaps") }, Control::Caps), QStringLiteral("custom"));
        QCOMPARE(value({ QStringLiteral("caps:super") }, Control::Caps), QStringLiteral("custom"));
        QCOMPARE(value({ QStringLiteral("caps:escape"), QStringLiteral("ctrl:nocaps") }, Control::Caps),
            QStringLiteral("custom"));
        QCOMPARE(value({ QStringLiteral("compose:prsc") }, Control::Compose), QStringLiteral("custom"));
        QCOMPARE(value({ QStringLiteral("ctrl:swap_ralt_rctl") }, Control::Corner), QStringLiteral("custom"));
        QCOMPARE(value({ QStringLiteral("altwin:ctrl_alt_win") }, Control::Corner), QStringLiteral("custom"));
        QCOMPARE(value({ QStringLiteral("numpad:microsoft") }, Control::Numpad), QStringLiteral("custom"));
        // ctrl:swapcaps is Caps Lock's kind only.
        QCOMPARE(value({ QStringLiteral("ctrl:swapcaps") }, Control::Corner), QStringLiteral("standard"));
    }

    void choosingReplacesOnlyItsKind()
    {
        const QStringList options = split(QStringLiteral("terminate:ctrl_alt_bksp,ctrl:swapcaps,compose:menu"));
        QCOMPARE(withValue(options, Control::Caps, QStringLiteral("escape")),
            split(QStringLiteral("terminate:ctrl_alt_bksp,compose:menu,caps:escape")));
        QCOMPARE(withValue(options, Control::Caps, QStringLiteral("capslock")),
            split(QStringLiteral("terminate:ctrl_alt_bksp,compose:menu")));
        QCOMPARE(withValue(options, Control::Compose, QStringLiteral("none")),
            split(QStringLiteral("terminate:ctrl_alt_bksp,ctrl:swapcaps")));
        QCOMPARE(
            withValue({}, Control::Numpad, QStringLiteral("digits")), QStringList { QStringLiteral("numpad:mac") });
        QCOMPARE(withValue({ QStringLiteral("altwin:swap_lalt_lwin") }, Control::Corner, QStringLiteral("ctrl-alt")),
            QStringList { QStringLiteral("ctrl:swap_lalt_lctl") });
        // Compose on Caps Lock takes the key from Caps Lock's options.
        QCOMPARE(withValue({ QStringLiteral("caps:escape") }, Control::Compose, QStringLiteral("caps")),
            QStringList { QStringLiteral("compose:caps") });
        // An unknown value, or "custom", changes nothing.
        QCOMPARE(withValue(options, Control::Caps, QStringLiteral("custom")), options);
        QCOMPARE(withValue(options, Control::Corner, QStringLiteral("sideways")), options);
    }

    void localedsOptionsAreRead()
    {
        QCOMPARE(fromLocaled(QStringLiteral("Section \"InputClass\"\n"
                                            "        Option \"XkbLayout\" \"it\"\n"
                                            "        Option \"XkbOptions\" \"caps:escape, compose:ralt\"\n"
                                            "EndSection\n")),
            (QStringList { QStringLiteral("caps:escape"), QStringLiteral("compose:ralt") }));
        QCOMPARE(fromLocaled(QStringLiteral("# Option \"XkbOptions\" \"caps:none\"\n")), QStringList {});
        QCOMPARE(fromLocaled(QString()), QStringList {});
        QCOMPARE(split(QStringLiteral(" , a,,b ,")), (QStringList { QStringLiteral("a"), QStringLiteral("b") }));
    }
};

QTEST_GUILESS_MAIN(KeyboardOptionsTest)
#include "keyboardoptions_test.moc"
