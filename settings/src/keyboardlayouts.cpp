// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keyboardlayouts.h"

#include <QCollator>
#include <QDebug>
#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QUrl>
#include <QVariantMap>
#include <QtEndian>

#include <algorithm>
#include <xkbcommon/xkbregistry.h>

KeyboardLayouts::KeyboardLayouts(QObject* parent)
    : QObject(parent)
    , m_localeDir(QStringLiteral("/usr/share/locale"))
    , m_flagDir(QStringLiteral(":/flags"))
{
}

KeyboardLayouts::KeyboardLayouts(
    const QStringList& includePaths, const QString& localeDir, const QString& flagDir, QObject* parent)
    : QObject(parent)
    , m_includePaths(includePaths)
    , m_localeDir(localeDir)
    , m_flagDir(flagDir)
{
}

void KeyboardLayouts::setLanguage(const QString& language)
{
    if (language == m_language) {
        return;
    }
    m_language = language;
    if (m_loaded) {
        translate();
    }
    ++m_revision;
    emit changed();
}

QHash<QString, QString> KeyboardLayouts::readCatalog(const QByteArray& data)
{
    // The GNU .mo layout: magic, revision, count, then the offsets of two
    // tables of (length, offset) pairs, originals and translations.
    QHash<QString, QString> catalog;
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
    const qsizetype size = data.size();
    if (size < 20) {
        return catalog;
    }
    bool big = false;
    if (qFromLittleEndian<quint32>(bytes) == 0x950412deU) {
        big = false;
    } else if (qFromBigEndian<quint32>(bytes) == 0x950412deU) {
        big = true;
    } else {
        return catalog;
    }
    const auto word = [&](qsizetype at) -> quint32 {
        return big ? qFromBigEndian<quint32>(bytes + at) : qFromLittleEndian<quint32>(bytes + at);
    };
    const quint32 count = word(8);
    const quint32 originals = word(12);
    const quint32 translations = word(16);
    if (count > size / 16 || originals > size - count * 8ULL || translations > size - count * 8ULL) {
        return catalog;
    }
    // The text of a (length, offset) pair: up to its first NUL (a plural's
    // other forms follow it), or empty when it doesn't fit.
    const auto text = [&](quint32 table, quint32 index) -> QString {
        const quint32 length = word(table + index * 8);
        const quint32 offset = word(table + index * 8 + 4);
        if (offset > size || length > size - offset) {
            return {};
        }
        const auto* start = reinterpret_cast<const char*>(bytes + offset);
        return QString::fromUtf8(start, qstrnlen(start, length));
    };
    catalog.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        const QString original = text(originals, i);
        const QString translation = text(translations, i);
        if (!original.isEmpty() && !translation.isEmpty()) {
            catalog.insert(original, translation);
        }
    }
    return catalog;
}

QString KeyboardLayouts::fold(const QString& text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString folded;
    folded.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing) {
            folded.append(c);
        }
    }
    return folded.toCaseFolded();
}

void KeyboardLayouts::load()
{
    m_loaded = true;
    // Exotic rules: evdev.extras.xml (EurKEY, APL, IPA, more variants).
    int flags = RXKB_CONTEXT_LOAD_EXOTIC_RULES;
    if (!m_includePaths.isEmpty()) {
        flags |= RXKB_CONTEXT_NO_DEFAULT_INCLUDES;
    }
    rxkb_context* context = rxkb_context_new(static_cast<rxkb_context_flags>(flags));
    if (!context) {
        qWarning("Keyboard layouts: no XKB registry");
        return;
    }
    for (const QString& path : std::as_const(m_includePaths)) {
        rxkb_context_include_path_append(context, QFile::encodeName(path).constData());
    }
    if (!rxkb_context_parse(context, "evdev")) {
        qWarning("Keyboard layouts: the XKB registry could not be read");
        rxkb_context_unref(context);
        return;
    }
    // The registry merges what the files say twice: an extras or user file
    // naming a layout again only adds variants.
    QHash<QString, int> groupOf;
    for (rxkb_layout* layout = rxkb_layout_first(context); layout; layout = rxkb_layout_next(layout)) {
        Entry entry;
        entry.layout = QString::fromUtf8(rxkb_layout_get_name(layout));
        entry.variant = QString::fromUtf8(rxkb_layout_get_variant(layout));
        entry.english = QString::fromUtf8(rxkb_layout_get_description(layout));
        for (rxkb_iso3166_code* code = rxkb_layout_get_iso3166_first(layout); code;
             code = rxkb_iso3166_code_next(code)) {
            entry.countries << QString::fromUtf8(rxkb_iso3166_code_get_code(code));
        }
        for (rxkb_iso639_code* code = rxkb_layout_get_iso639_first(layout); code; code = rxkb_iso639_code_next(code)) {
            entry.languages << QString::fromUtf8(rxkb_iso639_code_get_code(code));
        }
        if (entry.layout.isEmpty()) {
            continue;
        }
        auto group = groupOf.constFind(entry.layout);
        if (group == groupOf.constEnd()) {
            group = groupOf.insert(entry.layout, int(m_groups.size()));
            m_groups.append(Group {});
        }
        Group& into = m_groups[*group];
        const int index = int(m_entries.size());
        if (entry.variant.isEmpty()) {
            into.layout = index;
        } else {
            into.variants.append(index);
        }
        m_entries.append(entry);
    }
    rxkb_context_unref(context);
    // A layout listed only through its variants has no entry of its own:
    // its group is left out.
    m_groups.removeIf([](const Group& group) { return group.layout < 0; });
    // Flags: the entry's single country (the registry gives a variant that
    // names none its layout's). Several countries (latam, ara) or none
    // (Esperanto, Braille): no flag, the generic icon.
    for (Entry& entry : m_entries) {
        if (entry.countries.size() != 1 || m_flagDir.isEmpty()) {
            continue;
        }
        const QString file = m_flagDir + u'/' + entry.countries.first().toLower() + QStringLiteral(".svg");
        if (QFile::exists(file)) {
            entry.flag = file.startsWith(u':') ? QStringLiteral("qrc") + file : QUrl::fromLocalFile(file).toString();
        }
    }
    translate();
}

void KeyboardLayouts::translate()
{
    QHash<QString, QString> catalog;
    if (m_language != QLatin1String("en")) {
        // xkeyboard-config 2.45 renamed its domain; the old name stays as a
        // link on some systems, and is all there is on older ones.
        for (const char* domain : { "xkeyboard-config-2", "xkeyboard-config" }) {
            QFile file(m_localeDir + u'/' + m_language + QStringLiteral("/LC_MESSAGES/") + QLatin1String(domain)
                + QStringLiteral(".mo"));
            if (file.open(QIODevice::ReadOnly)) {
                catalog = readCatalog(file.read(4 * 1024 * 1024));
                if (!catalog.isEmpty()) {
                    break;
                }
            }
        }
    }
    // Languages and countries by code, in English and in their own language.
    QHash<QString, QString> words;
    const auto languageWords = [&](const QString& code) {
        auto found = words.constFind(u'l' + code);
        if (found == words.constEnd()) {
            const QLocale::Language language = QLocale::codeToLanguage(code, QLocale::ISO639Part3);
            // QLocale falls back to the default locale for a language it has
            // no data for: its own name only when it really is that one.
            const QLocale own(language);
            const QString text = language == QLocale::AnyLanguage ? QString()
                : own.language() == language ? QLocale::languageToString(language) + u' ' + own.nativeLanguageName()
                                             : QLocale::languageToString(language);
            found = words.insert(u'l' + code, text);
        }
        return *found;
    };
    const auto countryWords = [&](const QString& code) {
        auto found = words.constFind(u'c' + code);
        if (found == words.constEnd()) {
            const QLocale::Territory territory = QLocale::codeToTerritory(code);
            const QLocale own(QLocale::AnyLanguage, territory);
            const QString text = territory == QLocale::AnyTerritory ? QString()
                : own.territory() == territory
                ? QLocale::territoryToString(territory) + u' ' + own.nativeTerritoryName()
                : QLocale::territoryToString(territory);
            found = words.insert(u'c' + code, text);
        }
        return *found;
    };
    for (Entry& entry : m_entries) {
        entry.name = catalog.value(entry.english, entry.english);
        QStringList parts { entry.name, entry.english, entry.layout };
        if (!entry.variant.isEmpty()) {
            parts << entry.layout + u'(' + entry.variant + u')' << entry.variant;
        }
        for (const QString& code : std::as_const(entry.languages)) {
            parts << languageWords(code);
        }
        for (const QString& code : std::as_const(entry.countries)) {
            parts << countryWords(code);
        }
        entry.search = fold(parts.join(u' '));
    }
    QCollator collator(QLocale(m_language == QLatin1String("it") ? QLocale::Italian : QLocale::English));
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(m_groups.begin(), m_groups.end(), [&](const Group& a, const Group& b) {
        return collator.compare(m_entries.at(a.layout).name, m_entries.at(b.layout).name) < 0;
    });
}

QVariantList KeyboardLayouts::groups(const QString& query, int)
{
    if (!m_loaded) {
        load();
    }
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    const QStringList words = fold(query).split(spaces, Qt::SkipEmptyParts);
    const auto matches = [&](const Entry& entry) {
        return std::all_of(
            words.cbegin(), words.cend(), [&](const QString& word) { return entry.search.contains(word); });
    };
    const auto item = [](const Entry& entry) {
        return QVariantMap { { QStringLiteral("variant"), entry.variant }, { QStringLiteral("name"), entry.name },
            { QStringLiteral("flag"), entry.flag } };
    };
    QVariantList list;
    for (const Group& group : std::as_const(m_groups)) {
        const Entry& own = m_entries.at(group.layout);
        const bool groupMatches = matches(own);
        QVariantList variants;
        for (const int index : group.variants) {
            const Entry& variant = m_entries.at(index);
            if (groupMatches || matches(variant)) {
                variants.append(item(variant));
            }
        }
        if (groupMatches || !variants.isEmpty()) {
            list.append(QVariantMap { { QStringLiteral("layout"), own.layout }, { QStringLiteral("name"), own.name },
                { QStringLiteral("flag"), own.flag }, { QStringLiteral("matches"), groupMatches },
                { QStringLiteral("variants"), variants } });
        }
    }
    return list;
}

const KeyboardLayouts::Entry* KeyboardLayouts::find(const QString& layout, const QString& variant)
{
    if (!m_loaded) {
        load();
    }
    for (const Entry& entry : std::as_const(m_entries)) {
        if (entry.layout == layout && entry.variant == variant) {
            return &entry;
        }
    }
    return nullptr;
}

QString KeyboardLayouts::flag(const QString& layout, const QString& variant)
{
    const Entry* entry = find(layout, variant);
    return entry ? entry->flag : QString();
}

QString KeyboardLayouts::name(const QString& layout, const QString& variant, int)
{
    if (const Entry* entry = find(layout, variant)) {
        return entry->name;
    }
    return variant.isEmpty() ? layout : layout + QStringLiteral(" (") + variant + u')';
}

namespace {
// "it,us" and ",nodeadkeys": [{layout: "it", variant: ""}, {layout: "us",
// variant: "nodeadkeys"}].
QVariantList layoutList(const QString& layouts, const QString& variants)
{
    const QStringList names = layouts.split(u',', Qt::SkipEmptyParts);
    const QStringList kinds = variants.split(u',');
    QVariantList list;
    for (qsizetype i = 0; i < names.size(); ++i) {
        list.append(QVariantMap { { QStringLiteral("layout"), names.at(i).trimmed() },
            { QStringLiteral("variant"), i < kinds.size() ? kinds.at(i).trimmed() : QString() } });
    }
    return list;
}
} // namespace

QVariantMap KeyboardLayouts::sourceFrom(const QString& envLayout, const QString& envVariant, const QString& localed)
{
    if (!envLayout.isEmpty()) {
        return { { QStringLiteral("source"), QStringLiteral("environment") },
            { QStringLiteral("layouts"), layoutList(envLayout, envVariant) } };
    }
    // Option "XkbLayout" "it,us", as the compositor reads it.
    static const QRegularExpression option(QStringLiteral(R"re(^\s*Option\s+"([^"]*)"\s+"([^"]*)")re"));
    QString layout;
    QString variant;
    for (const QString& line : localed.split(u'\n')) {
        const QRegularExpressionMatch match = option.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        if (match.captured(1) == QLatin1String("XkbLayout")) {
            layout = match.captured(2);
        } else if (match.captured(1) == QLatin1String("XkbVariant")) {
            variant = match.captured(2);
        }
    }
    const QVariantList layouts = layoutList(layout, variant);
    if (layouts.isEmpty()) {
        return { { QStringLiteral("source"), QStringLiteral("default") },
            { QStringLiteral("layouts"),
                QVariantList { QVariantMap { { QStringLiteral("layout"), QStringLiteral("us") },
                    { QStringLiteral("variant"), QString() } } } } };
    }
    return { { QStringLiteral("source"), QStringLiteral("system") }, { QStringLiteral("layouts"), layouts } };
}

QVariantMap KeyboardLayouts::source() const
{
    QString localed;
    QFile file(QStringLiteral("/etc/X11/xorg.conf.d/00-keyboard.conf"));
    if (file.open(QIODevice::ReadOnly)) {
        localed = QString::fromUtf8(file.read(64 * 1024));
    }
    return sourceFrom(qEnvironmentVariable("XKB_DEFAULT_LAYOUT"), qEnvironmentVariable("XKB_DEFAULT_VARIANT"), localed);
}
