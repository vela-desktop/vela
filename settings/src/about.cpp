// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "processlaunch.h"
#include "about.h"
#include "systeminfo.h"

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

/** Query PCI graphics devices through an external process with the user's environment. */
QStringList graphics()
{
    vela::process::ExternalProcess lspci;
    lspci.start(QStringLiteral("lspci"), QStringList {});
    lspci.waitForFinished(2000);
    return systeminfo::graphicsCards(QString::fromUtf8(lspci.readAllStandardOutput()));
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
    m_device << row(QCoreApplication::translate("About", "Processor"),
        systeminfo::cpuName(readFile(QStringLiteral("/proc/cpuinfo")), QSysInfo::currentCpuArchitecture()));
    const std::optional<double> memory = systeminfo::memoryGiB(readFile(QStringLiteral("/proc/meminfo")));
    m_device << row(QCoreApplication::translate("About", "Installed RAM"),
        memory ? QLocale().toString(*memory, 'f', 1) + QCoreApplication::translate("About", " GB (usable)")
               : QString());
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

    QString os = systeminfo::osReleaseValue(readFile(QStringLiteral("/etc/os-release")), QStringLiteral("PRETTY_NAME"));
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
