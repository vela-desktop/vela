// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What Settings > About reads from the system files and tools, as text in,
// values out: the parsing, apart from the reading, so that tests can feed it
// what other machines print.

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <optional>

namespace systeminfo {

// From /proc/cpuinfo: "AMD Ryzen 9 9950X 16-Core Processor   (32 thread)",
// or `fallback` (the architecture) when no model name is given.
inline QString cpuName(const QString& cpuinfo, const QString& fallback)
{
    static const QRegularExpression model(
        QStringLiteral("^model name\\s*:\\s*(.+)$"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = model.match(cpuinfo);
    const QString name = m.hasMatch() ? m.captured(1).trimmed() : fallback;
    const int threads = int(cpuinfo.count(QStringLiteral("processor\t:")));
    return threads > 0 ? name + QStringLiteral("   (") + QString::number(threads) + QStringLiteral(" thread)") : name;
}

// From /proc/meminfo: MemTotal in GiB.
inline std::optional<double> memoryGiB(const QString& meminfo)
{
    static const QRegularExpression total(QStringLiteral("MemTotal:\\s*(\\d+) kB"));
    const QRegularExpressionMatch m = total.match(meminfo);
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    return m.captured(1).toDouble() / 1024.0 / 1024.0;
}

// From lspci, the display controllers by name: "03:00.0 VGA compatible
// controller: Advanced Micro Devices, Inc. [AMD/ATI] Navi 48 [Radeon RX
// 9070 XT] (rev c0)" is "AMD Radeon RX 9070 XT".
inline QStringList graphicsCards(const QString& lspci)
{
    static const QRegularExpression revision(QStringLiteral("\\s*\\(rev [0-9a-f]+\\)$"));
    static const QRegularExpression marketingName(QStringLiteral("\\[([^\\]]+)\\]$"));
    QStringList out;
    for (const QString& line : lspci.split(u'\n')) {
        if (!line.contains(QLatin1String("VGA compatible")) && !line.contains(QLatin1String("3D controller"))
            && !line.contains(QLatin1String("Display controller"))) {
            continue;
        }
        QString name = line.section(QStringLiteral(": "), 1).remove(revision);
        // The marketing name in square brackets, when present, says more.
        const QRegularExpressionMatch bracket = marketingName.match(name);
        if (name.startsWith(QLatin1String("Advanced Micro Devices"))) {
            name =
                QStringLiteral("AMD ") + (bracket.hasMatch() ? bracket.captured(1) : name.section(u']', 1).trimmed());
        } else if (name.startsWith(QLatin1String("NVIDIA"))) {
            name = QStringLiteral("NVIDIA ") + (bracket.hasMatch() ? bracket.captured(1) : name.section(u' ', 2));
        } else if (name.startsWith(QLatin1String("Intel"))) {
            name = QStringLiteral("Intel ") + (bracket.hasMatch() ? bracket.captured(1) : name.section(u' ', 2));
        }
        out << name;
    }
    return out;
}

// From /etc/os-release (KEY=value or KEY="value", one per line): the value
// of `key`, or empty.
inline QString osReleaseValue(const QString& text, const QString& key)
{
    static const QRegularExpression assignment(
        QStringLiteral("^([A-Za-z0-9_]+)=\"?([^\"\\n]*)\"?$"), QRegularExpression::MultilineOption);
    for (auto it = assignment.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (m.captured(1) == key) {
            return m.captured(2);
        }
    }
    return {};
}

} // namespace systeminfo
