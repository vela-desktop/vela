// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keyboardlayouts.h"

#include <QCollator>
#include <QFile>
#include <QRegularExpression>
#include <QVariantMap>
#include <QXmlStreamReader>

#include <algorithm>

void KeyboardLayouts::load()
{
    QFile file(QStringLiteral("/usr/share/X11/xkb/rules/evdev.xml"));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    // <layout><configItem><name>it</name><description>Italian</description>
    // </configItem><variantList><variant><configItem>...</configItem></variant>...
    // The descriptions are in English: libxkbcommon has no translations,
    // xkeyboard-config takes them from its gettext domain. They're left as
    // they are here.
    QXmlStreamReader xml(&file);
    QString layout;
    QString layoutName;
    bool inVariant = false;
    QString name;
    QString description;
    QStringList path;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            path << xml.name().toString();
            if (xml.name() == QLatin1String("variant")) {
                inVariant = true;
                name.clear();
                description.clear();
            } else if (xml.name() == QLatin1String("layout")) {
                layout.clear();
                layoutName.clear();
            } else if (xml.name() == QLatin1String("name") && path.size() >= 2 && path[path.size() - 2] == QLatin1String("configItem")) {
                name = xml.readElementText();
                path.removeLast();
            } else if (xml.name() == QLatin1String("description") && path.size() >= 2
                && path[path.size() - 2] == QLatin1String("configItem")) {
                description = xml.readElementText();
                path.removeLast();
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QLatin1String("configItem") && path.contains(QLatin1String("layoutList"))) {
                if (inVariant) {
                    m_all.append(QVariantMap { { QStringLiteral("layout"), layout }, { QStringLiteral("variant"), name },
                        { QStringLiteral("name"), description } });
                } else if (!path.contains(QLatin1String("variantList"))) {
                    layout = name;
                    layoutName = description;
                    m_all.append(QVariantMap { { QStringLiteral("layout"), layout }, { QStringLiteral("variant"), QString() },
                        { QStringLiteral("name"), description } });
                }
            } else if (xml.name() == QLatin1String("variant")) {
                inVariant = false;
            }
            if (!path.isEmpty()) {
                path.removeLast();
            }
        }
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(m_all.begin(), m_all.end(), [&](const QVariant& a, const QVariant& b) {
        return collator.compare(a.toMap()[QStringLiteral("name")].toString(), b.toMap()[QStringLiteral("name")].toString()) < 0;
    });
}

QVariantList KeyboardLayouts::all()
{
    if (m_all.isEmpty()) {
        load();
    }
    return m_all;
}

QString KeyboardLayouts::name(const QString& layout, const QString& variant)
{
    for (const QVariant& value : all()) {
        const QVariantMap entry = value.toMap();
        if (entry[QStringLiteral("layout")] == layout && entry[QStringLiteral("variant")].toString() == variant) {
            return entry[QStringLiteral("name")].toString();
        }
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
