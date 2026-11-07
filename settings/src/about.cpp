// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "processlaunch.h"
#include "about.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QVariantMap>

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const QString hostnamed = QStringLiteral("org.freedesktop.hostname1");
const QString hostnamedPath = QStringLiteral("/org/freedesktop/hostname1");

QVariantMap row(const QString& label, const QString& value)
{
    return { { QStringLiteral("label"), label }, { QStringLiteral("value"), value } };
}

QString readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString cpuName()
{
    const QString cpuinfo = readFile(QStringLiteral("/proc/cpuinfo"));
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^model name\\s*:\\s*(.+)$"),
        QRegularExpression::MultilineOption).match(cpuinfo);
    QString name = m.hasMatch() ? m.captured(1).trimmed() : QSysInfo::currentCpuArchitecture();
    const int threads = cpuinfo.count(QStringLiteral("processor\t:"));
    return threads > 0 ? name + QStringLiteral("   (") + QString::number(threads) + QStringLiteral(" thread)") : name;
}

QString memory()
{
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("MemTotal:\\s*(\\d+) kB"))
                                          .match(readFile(QStringLiteral("/proc/meminfo")));
    if (!m.hasMatch()) {
        return {};
    }
    const double gib = m.captured(1).toDouble() / 1024.0 / 1024.0;
    return QLocale().toString(gib, 'f', 1) + QCoreApplication::translate("About", " GB (usable)");
}

/** Query PCI graphics devices through an external process with the user's environment. */
QStringList graphics()
{
    // lspci: "03:00.0 VGA compatible controller: Advanced Micro Devices, Inc.
    // [AMD/ATI] Navi 48 [Radeon RX 9070...]"
    vela::process::ExternalProcess lspci;
    lspci.start(QStringLiteral("lspci"), QStringList {});
    lspci.waitForFinished(2000);
    QStringList out;
    for (const QString& line : QString::fromUtf8(lspci.readAllStandardOutput()).split(u'\n')) {
        if (!line.contains(QLatin1String("VGA compatible")) && !line.contains(QLatin1String("3D controller"))
            && !line.contains(QLatin1String("Display controller"))) {
            continue;
        }
        QString name = line.section(QStringLiteral(": "), 1).remove(QRegularExpression(QStringLiteral("\\s*\\(rev [0-9a-f]+\\)$")));
        // The marketing name in square brackets, when present, says more.
        const QRegularExpressionMatch bracket = QRegularExpression(QStringLiteral("\\[([^\\]]+)\\]$")).match(name);
        if (name.startsWith(QLatin1String("Advanced Micro Devices"))) {
            name = QStringLiteral("AMD ") + (bracket.hasMatch() ? bracket.captured(1) : name.section(u']', 1).trimmed());
        } else if (name.startsWith(QLatin1String("NVIDIA"))) {
            name = QStringLiteral("NVIDIA ") + (bracket.hasMatch() ? bracket.captured(1) : name.section(u' ', 2));
        } else if (name.startsWith(QLatin1String("Intel"))) {
            name = QStringLiteral("Intel ") + (bracket.hasMatch() ? bracket.captured(1) : name.section(u' ', 2));
        }
        out << name;
    }
    return out;
}

QString osRelease(const QString& key)
{
    const QString text = readFile(QStringLiteral("/etc/os-release"));
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^") + key + QStringLiteral("=\"?([^\"\\n]*)\"?$"),
        QRegularExpression::MultilineOption).match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

About::About(QObject* parent)
    : QObject(parent)
{
    QDBusInterface host(hostnamed, hostnamedPath, hostnamed, QDBusConnection::systemBus());
    m_hostname = host.property("StaticHostname").toString();
    if (m_hostname.isEmpty()) {
        m_hostname = QSysInfo::machineHostName();
    }
    const QString vendor = host.property("HardwareVendor").toString();
    const QString hardware = host.property("HardwareModel").toString();
    m_model = (vendor + u' ' + hardware).trimmed();
    const QString chassis = host.property("Chassis").toString();
    m_chassisIcon = chassis == QLatin1String("laptop") || chassis == QLatin1String("convertible")
        ? QStringLiteral("computer-laptop")
        : QStringLiteral("computer");

    m_device << row(QCoreApplication::translate("About", "Device name"), m_hostname);
    m_device << row(QCoreApplication::translate("About", "Processor"), cpuName());
    m_device << row(QCoreApplication::translate("About", "Installed RAM"), memory());
    const QStringList gpus = graphics();
    for (const QString& gpu : gpus) {
        m_device << row(QCoreApplication::translate("About", "Graphics card"), gpu);
    }
    if (!m_model.isEmpty()) {
        m_device << row(QCoreApplication::translate("About", "Model"), m_model);
    }
    const QString machineId = readFile(QStringLiteral("/etc/machine-id")).trimmed();
    if (!machineId.isEmpty()) {
        m_device << row(QCoreApplication::translate("About", "Device ID"), machineId);
    }
    m_device << row(QCoreApplication::translate("About", "System type"),
        QCoreApplication::translate("About", "%1-bit operating system, %2 processor").arg(QSysInfo::WordSize).arg(QSysInfo::currentCpuArchitecture()));

    QString os = osRelease(QStringLiteral("PRETTY_NAME"));
    if (os.isEmpty()) {
        os = QSysInfo::prettyProductName();
    }
    m_system << row(QCoreApplication::translate("About", "Operating system"), os);
    m_system << row(QStringLiteral("Desktop"), QStringLiteral("Vela ") + QStringLiteral(VELA_VERSION));
    m_system << row(QStringLiteral("Kernel"), QStringLiteral("Linux ") + QSysInfo::kernelVersion());
    // "Installed on": when the filesystem root was created.
    struct statx info {};
    if (statx(AT_FDCWD, "/", 0, STATX_BTIME, &info) == 0 && (info.stx_mask & STATX_BTIME)) {
        const QDateTime born = QDateTime::fromSecsSinceEpoch(info.stx_btime.tv_sec);
        m_system << row(QCoreApplication::translate("About", "Installed on"), QLocale().toString(born.date(), QStringLiteral("d MMMM yyyy")));
    }
    m_system << row(QStringLiteral("Qt"), QString::fromLatin1(qVersion()));
}

QString About::userName() const
{
    if (const passwd* pw = getpwuid(getuid())) {
        const QString gecos = QString::fromLocal8Bit(pw->pw_gecos).section(u',', 0, 0).trimmed();
        return gecos.isEmpty() ? QString::fromLocal8Bit(pw->pw_name) : gecos;
    }
    return qEnvironmentVariable("USER");
}

void About::rename(const QString& name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty() || clean == m_hostname) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(hostnamed, hostnamedPath, hostnamed, QStringLiteral("SetStaticHostname"));
    call << clean << true; // interactive: polkit asks for the password
    call.setInteractiveAuthorizationAllowed(true);
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 120000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, clean](QDBusPendingCallWatcher* w) {
        const QDBusPendingReply<> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
            emit renameFailed(reply.error().name().contains(QLatin1String("InvalidArgs"))
                    ? QCoreApplication::translate("About", "The name can only contain letters, numbers and hyphens.")
                    : QCoreApplication::translate("About", "Couldn't rename the PC: ") + reply.error().message());
            return;
        }
        m_hostname = clean;
        if (!m_device.isEmpty()) {
            m_device[0] = row(QCoreApplication::translate("About", "Device name"), clean);
        }
        emit hostnameChanged();
    });
}

QString About::asText() const
{
    QString text = QCoreApplication::translate("About", "Device specifications\n");
    for (const QVariant& r : m_device) {
        text += r.toMap()[QStringLiteral("label")].toString() + u'\t' + r.toMap()[QStringLiteral("value")].toString() + u'\n';
    }
    text += QCoreApplication::translate("About", "\nSystem specifications\n");
    for (const QVariant& r : m_system) {
        text += r.toMap()[QStringLiteral("label")].toString() + u'\t' + r.toMap()[QStringLiteral("value")].toString() + u'\n';
    }
    return text;
}
