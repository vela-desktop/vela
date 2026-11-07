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
    using QProcess::QProcess;

    /** Launch with the current filtered environment, including new activation tokens. */
    void start(const QString& program, const QStringList& arguments = {}, OpenMode mode = ReadWrite)
    {
        setProcessEnvironment(externalProcessEnvironment());
        QProcess::start(program, arguments, mode);
    }

    /** Start the configured program with a fresh environment on every reuse. */
    void start(OpenMode mode = ReadWrite)
    {
        setProcessEnvironment(externalProcessEnvironment());
        QProcess::start(mode);
    }

    /** Split a Qt command line and launch it with the current filtered environment. */
    void startCommand(const QString& command, OpenMode mode = ReadWrite)
    {
        setProcessEnvironment(externalProcessEnvironment());
        QProcess::startCommand(command, mode);
    }

    /** Detach the configured program after refreshing the child environment. */
    bool startDetached(qint64* pid = nullptr)
    {
        setProcessEnvironment(externalProcessEnvironment());
        return QProcess::startDetached(pid);
    }

    /** Preserve the same environment boundary when QIODevice opens the process. */
    bool open(OpenMode mode = ReadWrite) override
    {
        setProcessEnvironment(externalProcessEnvironment());
        return QProcess::open(mode);
    }
};

/** Start an independent program without inheriting Vela's private defaults. */
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
