// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "processenvironment.h"

#include <QProcess>

// Restore the user's environment for programs Vela opens. Direct
// QProcess::startDetached() and Qt.openUrlExternally() inherit Qt's
// process-only grayscale setting; prefer these entry points instead.
namespace vela::process {

class ExternalProcess : public QProcess {
public:
    explicit ExternalProcess(QObject* parent = nullptr)
        : QProcess(parent)
    {
        setProcessEnvironment(externalProcessEnvironment());
    }
};

inline bool startDetached(const QString& program, const QStringList& arguments = {},
    const QString& directory = {}, qint64* pid = nullptr)
{
    ExternalProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    if (!directory.isEmpty()) {
        process.setWorkingDirectory(directory);
    }
    return process.startDetached(pid);
}

} // namespace vela::process
