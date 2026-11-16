// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QProcessEnvironment>

namespace vela::process {

// Set a process-local default before starting application threads. Respect
// inherited values, including empty ones; remember only defaults we inject.
void setPrivateEnvironmentDefault(const char* name, const QByteArray& value);

// Start from the current environment, retaining later changes (activation
// tokens, for example), and remove only our unchanged private defaults.
QProcessEnvironment externalProcessEnvironment();

} // namespace vela::process
