// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' keyboard layouts (src/keyboardlayouts.h): the catalogue from a
// small XKB registry (tests/keyboard: base, extras and a user's rules), its
// translations, groups and search (docs/keyboard.md §5.1-5.2), and where the
// layout in use comes from when Settings has none (§5.5).

#include "keyboardlayouts.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

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

// A gettext catalog of `pairs` (original, translation), in the GNU .mo
// layout, little- or big-endian.
QByteArray catalog(QList<QPair<QByteArray, QByteArray>> pairs, bool bigEndian = false)
{
    std::sort(pairs.begin(), pairs.end());
    const quint32 count = quint32(pairs.size());
    const quint32 originals = 28;
    const quint32 translations = originals + count * 8;
    QByteArray head(28 + count * 16, '\0');
    QByteArray strings;
    const quint32 base = quint32(head.size());
    const auto put = [&](quint32 at, quint32 value) {
        if (bigEndian) {
            qToBigEndian(value, head.data() + at);
        } else {
            qToLittleEndian(value, head.data() + at);
        }
    };
    put(0, 0x950412deU);
    put(8, count);
    put(12, originals);
    put(16, translations);
    for (quint32 i = 0; i < count; ++i) {
        put(originals + i * 8, quint32(pairs.at(i).first.size()));
        put(originals + i * 8 + 4, base + quint32(strings.size()));
        strings += pairs.at(i).first + '\0';
    }
    for (quint32 i = 0; i < count; ++i) {
        put(translations + i * 8, quint32(pairs.at(i).second.size()));
        put(translations + i * 8 + 4, base + quint32(strings.size()));
        strings += pairs.at(i).second + '\0';
    }
    return head + strings;
}

QStringList names(const QVariantList& groups)
{
    QStringList list;
    for (const QVariant& group : groups) {
        list << group.toMap().value(QStringLiteral("name")).toString();
    }
    return list;
}

QStringList variants(const QVariantList& groups, const QString& layout)
{
    for (const QVariant& value : groups) {
        const QVariantMap group = value.toMap();
        if (group.value(QStringLiteral("layout")) == layout) {
            QStringList list;
            for (const QVariant& variant : group.value(QStringLiteral("variants")).toList()) {
                list << variant.toMap().value(QStringLiteral("variant")).toString();
            }
            return list;
        }
    }
    return { QStringLiteral("<no group>") };
}

bool groupMatches(const QVariantList& groups, const QString& layout)
{
    for (const QVariant& value : groups) {
        const QVariantMap group = value.toMap();
        if (group.value(QStringLiteral("layout")) == layout) {
            return group.value(QStringLiteral("matches")).toBool();
        }
    }
    return false;
}

const QString fixtures = QStringLiteral(KEYBOARD_FIXTURES);

} // namespace

class KeyboardLayoutsTest : public QObject {
    Q_OBJECT

private slots:
    void theRegistryGivesBaseExtrasAndUserLayouts()
    {
        KeyboardLayouts layouts({ fixtures + QStringLiteral("/user"), fixtures }, QString());
        const QVariantList groups = layouts.groups(QString());
        QCOMPARE(names(groups),
            (QStringList { QStringLiteral("Ancient"), QStringLiteral("English (US)"), QStringLiteral("Esperanto"),
                QStringLiteral("EurKEY (US)"), QStringLiteral("German"), QStringLiteral("Italian"),
                QStringLiteral("Russian"), QStringLiteral("Vela test layout") }));
        // The extras' variant joins its layout's group, after the base ones.
        QCOMPARE(variants(groups, QStringLiteral("it")),
            (QStringList { QStringLiteral("us"), QStringLiteral("nodeadkeys"), QStringLiteral("ibm") }));
        QCOMPARE(layouts.name(QStringLiteral("it"), QStringLiteral("ibm")), QStringLiteral("Italian (IBM 142)"));
        QCOMPARE(layouts.name(QStringLiteral("xx"), QStringLiteral("yy")), QStringLiteral("xx (yy)"));
    }

    void namesFollowVelasLanguage()
    {
        QTemporaryDir locale;
        QVERIFY(QDir().mkpath(locale.path() + QStringLiteral("/it/LC_MESSAGES")));
        QFile file(locale.path() + QStringLiteral("/it/LC_MESSAGES/xkeyboard-config-2.mo"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(catalog({ { "Italian", "Italiana" }, { "German", "Tedesca" }, { "English (US)", "Inglese (USA)" },
            { "Italian (US)", "Italiana (USA)" } }));
        file.close();
        KeyboardLayouts layouts({ fixtures }, locale.path());
        QCOMPARE(layouts.name(QStringLiteral("it"), QString()), QStringLiteral("Italian"));
        QSignalSpy changed(&layouts, &KeyboardLayouts::changed);
        layouts.setLanguage(QStringLiteral("it"));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(layouts.revision(), 1);
        QCOMPARE(layouts.name(QStringLiteral("it"), QString()), QStringLiteral("Italiana"));
        QCOMPARE(layouts.name(QStringLiteral("it"), QStringLiteral("us")), QStringLiteral("Italiana (USA)"));
        // Untranslated names stay in English; the order is the translated one.
        QCOMPARE(names(layouts.groups(QString())),
            (QStringList { QStringLiteral("Ancient"), QStringLiteral("Esperanto"), QStringLiteral("EurKEY (US)"),
                QStringLiteral("Inglese (USA)"), QStringLiteral("Italiana"), QStringLiteral("Russian"),
                QStringLiteral("Tedesca") }));
        layouts.setLanguage(QStringLiteral("it"));
        QCOMPARE(changed.count(), 1);
        layouts.setLanguage(QStringLiteral("en"));
        QCOMPARE(layouts.name(QStringLiteral("de"), QString()), QStringLiteral("German"));
    }

    void searchFindsNamesCodesLanguagesAndCountries()
    {
        QTemporaryDir locale;
        QVERIFY(QDir().mkpath(locale.path() + QStringLiteral("/it/LC_MESSAGES")));
        QFile file(locale.path() + QStringLiteral("/it/LC_MESSAGES/xkeyboard-config.mo"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(catalog({ { "German", "Tedesca" } }));
        file.close();
        KeyboardLayouts layouts({ fixtures }, locale.path());
        layouts.setLanguage(QStringLiteral("it"));
        const auto found = [&](const QString& query) { return names(layouts.groups(query)); };
        QCOMPARE(found(QStringLiteral("tedesca")), QStringList { QStringLiteral("Tedesca") }); // translated
        QCOMPARE(found(QStringLiteral("GERMAN")), QStringList { QStringLiteral("Tedesca") }); // English, any case
        QCOMPARE(found(QStringLiteral("tedésca")), QStringList { QStringLiteral("Tedesca") }); // accents
        QCOMPARE(found(QStringLiteral("deutsch")), QStringList { QStringLiteral("Tedesca") }); // language, its own name
        QCOMPARE(found(QStringLiteral("deutschland")), QStringList { QStringLiteral("Tedesca") }); // country, its own
        QCOMPARE(found(QStringLiteral("italiano")),
            (QStringList { QStringLiteral("EurKEY (US)"), QStringLiteral("Italian") }));
        QCOMPARE(found(QStringLiteral("italia")),
            (QStringList { QStringLiteral("EurKEY (US)"), QStringLiteral("Italian") }));
        QCOMPARE(found(QStringLiteral("de")).contains(QStringLiteral("Tedesca")), true); // the code
        QCOMPARE(found(QStringLiteral("nothing like this")), QStringList {});
        // A language Qt has no data for (Gothic, Ugaritic) is found by its
        // English name, never by the default locale's: with an Italian
        // default, "italiano" must not find Ancient.
        const QLocale previous;
        QLocale::setDefault(QLocale(QLocale::Italian, QLocale::Italy));
        KeyboardLayouts italianDefault({ fixtures }, QString());
        QCOMPARE(names(italianDefault.groups(QStringLiteral("italiano"))),
            (QStringList { QStringLiteral("EurKEY (US)"), QStringLiteral("Italian") }));
        QCOMPARE(names(italianDefault.groups(QStringLiteral("gothic"))), QStringList { QStringLiteral("Ancient") });
        QLocale::setDefault(previous);

        // Found through a variant only: the group with just that variant.
        const QVariantList intl = layouts.groups(QStringLiteral("english intl"));
        QCOMPARE(names(intl), QStringList { QStringLiteral("English (US)") });
        QVERIFY(!groupMatches(intl, QStringLiteral("us")));
        QCOMPARE(variants(intl, QStringLiteral("us")), QStringList { QStringLiteral("intl") });
        QCOMPARE(variants(layouts.groups(QStringLiteral("it(us)")), QStringLiteral("it")),
            QStringList { QStringLiteral("us") });
        // Found through the layout: every variant, and the group matches.
        const QVariantList italian = layouts.groups(QStringLiteral("italian"));
        QVERIFY(groupMatches(italian, QStringLiteral("it")));
        QCOMPARE(variants(italian, QStringLiteral("it")),
            (QStringList { QStringLiteral("us"), QStringLiteral("nodeadkeys"), QStringLiteral("ibm") }));
    }

    void catalogsAreReadInBothByteOrdersAndBrokenOnesAreEmpty()
    {
        const QByteArray little = catalog({ { "German", "Tedesca" }, { "Russian", "Russa" } });
        QCOMPARE(KeyboardLayouts::readCatalog(little).value(QStringLiteral("Russian")), QStringLiteral("Russa"));
        const QByteArray big = catalog({ { "German", "Tedesca" } }, true);
        QCOMPARE(KeyboardLayouts::readCatalog(big).value(QStringLiteral("German")), QStringLiteral("Tedesca"));
        // A plural's other forms after a NUL: the first one.
        QCOMPARE(
            KeyboardLayouts::readCatalog(catalog({ { QByteArray("key\0keys", 8), QByteArray("tasto\0tasti", 11) } }))
                .value(QStringLiteral("key")),
            QStringLiteral("tasto"));
        QVERIFY(
            KeyboardLayouts::readCatalog(little.left(little.size() - 4)).value(QStringLiteral("Russian")).isEmpty());
        QVERIFY(KeyboardLayouts::readCatalog(little.left(30)).isEmpty());
        QVERIFY(KeyboardLayouts::readCatalog(QByteArray(64, 'x')).isEmpty());
        QByteArray huge = little;
        qToLittleEndian<quint32>(0x7fffffffU, huge.data() + 8); // a count that can't fit
        QVERIFY(KeyboardLayouts::readCatalog(huge).isEmpty());
    }

    void environmentWinsOverEverything()
    {
        const QVariantMap source =
            KeyboardLayouts::sourceFrom(QStringLiteral("de"), QStringLiteral("nodeadkeys"), localed);
        QCOMPARE(source.value(QStringLiteral("source")).toString(), QStringLiteral("environment"));
        QCOMPARE(layouts(source), QStringLiteral("de(nodeadkeys)"));
    }

    void eachEnvironmentFieldReplacesItsOwn()
    {
        // A variant alone: localed's layouts with it, as the compositor.
        QVariantMap source = KeyboardLayouts::sourceFrom(QString(), QStringLiteral("dvorak"),
            QStringLiteral("Option \"XkbLayout\" \"us\"\n"));
        QCOMPARE(source.value(QStringLiteral("source")).toString(), QStringLiteral("system"));
        QCOMPARE(layouts(source), QStringLiteral("us(dvorak)"));
        // A layout alone keeps localed's variant.
        source = KeyboardLayouts::sourceFrom(QStringLiteral("us"), QString(),
            QStringLiteral("Option \"XkbLayout\" \"us\"\nOption \"XkbVariant\" \"dvorak\"\n"));
        QCOMPARE(source.value(QStringLiteral("source")).toString(), QStringLiteral("environment"));
        QCOMPARE(layouts(source), QStringLiteral("us(dvorak)"));
        // Nothing configured: us with the variant.
        source = KeyboardLayouts::sourceFrom(QString(), QStringLiteral("dvorak"), QString());
        QCOMPARE(source.value(QStringLiteral("source")).toString(), QStringLiteral("default"));
        QCOMPARE(layouts(source), QStringLiteral("us(dvorak)"));
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
