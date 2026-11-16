// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "processenvironment.h"

#include <QMap>

#include <mutex>

namespace {
QMap<QByteArray, QByteArray> privateDefaults;
std::mutex environmentMutex;
} // namespace

namespace vela::process {

/** Register an injected default while preserving inherited values, including empty ones. */
void setPrivateEnvironmentDefault(const char* name, const QByteArray& value)
{
    const std::lock_guard lock(environmentMutex);
    if (!qEnvironmentVariableIsSet(name) && qputenv(name, value)) {
        privateDefaults.insert(name, value);
    }
}

/** Copy the current environment and remove only unchanged defaults owned by Vela. */
QProcessEnvironment externalProcessEnvironment()
{
    const std::lock_guard lock(environmentMutex);
    auto environment = QProcessEnvironment::systemEnvironment();
    for (auto it = privateDefaults.cbegin(); it != privateDefaults.cend(); ++it) {
        const QString name = QString::fromLatin1(it.key());
        if (environment.contains(name) && environment.value(name) == QString::fromLocal8Bit(it.value())) {
            environment.remove(name);
        }
    }
    return environment;
}

} // namespace vela::process
