// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' keyboard layouts (src/keyboardlayouts.h): where the layout in use
// comes from when Settings has none (docs/keyboard.md §5.5).

#include "keyboardlayouts.h"

#include <QTest>

namespace {

QString layouts(const QVariantMap& source)
{
    QStringList list;
    for (const QVariant& value : source.value(QStringLiteral("layouts")).toList()) {
        const QVariantMap entry = value.toMap();
        const QString variant = entry.value(QStringLiteral("variant")).toString();
        list << entry.value(QStringLiteral("layout")).toString()
                + (variant.isEmpty() ? QString() : u'(' + variant + u')');
    }
    return list.join(u',');
}

const QString localed = QStringLiteral(R"(# Written by systemd-localed(8), read by systemd-localed and Xorg.
Section "InputClass"
        Identifier "system-keyboard"
        MatchIsKeyboard "on"
        Option "XkbLayout" "it,us"
        Option "XkbVariant" ",intl"
        Option "XkbModel" "pc105"
EndSection
)");

} // namespace

class KeyboardLayoutsTest : public QObject {
    Q_OBJECT

private slots:
    void environmentWinsOverEverything()
    {
        const QVariantMap source =
            KeyboardLayouts::sourceFrom(QStringLiteral("de"), QStringLiteral("nodeadkeys"), localed);
        QCOMPARE(source.value(QStringLiteral("source")).toString(), QStringLiteral("environment"));
        QCOMPARE(layouts(source), QStringLiteral("de(nodeadkeys)"));
    }

    void systemdLocaledGivesEveryLayoutAndVariant()
    {
        const QVariantMap source = KeyboardLayouts::sourceFrom(QString(), QString(), localed);
        QCOMPARE(source.value(QStringLiteral("source")).toString(), QStringLiteral("system"));
        QCOMPARE(layouts(source), QStringLiteral("it,us(intl)"));
    }

    void nothingConfiguredIsEnglishUs()
    {
        for (const QString& text : { QString(), QStringLiteral("Section \"InputClass\"\nEndSection\n"),
                 QStringLiteral("Option \"XkbModel\" \"pc105\"\n") }) {
            const QVariantMap source = KeyboardLayouts::sourceFrom(QString(), QString(), text);
            QCOMPARE(source.value(QStringLiteral("source")).toString(), QStringLiteral("default"));
            QCOMPARE(layouts(source), QStringLiteral("us"));
        }
    }

    void commentedOptionsAreIgnored()
    {
        const QVariantMap source = KeyboardLayouts::sourceFrom(
            QString(), QString(), QStringLiteral("#   Option \"XkbLayout\" \"fr\"\n    Option \"XkbLayout\" \"es\"\n"));
        QCOMPARE(layouts(source), QStringLiteral("es"));
    }
};

QTEST_GUILESS_MAIN(KeyboardLayoutsTest)
#include "keyboardlayouts_test.moc"
