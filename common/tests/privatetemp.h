// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace vela::test {

// Fixtures live beneath a private directory in this build, not in /tmp.
// Never fall back to the current directory when creation fails.
inline std::string privateTempDirectory(const std::string& prefix,
    const char* base = VELA_TEST_PRIVATE_DIR)
{
    if (prefix.empty() || prefix.find('/') != std::string::npos) {
        throw std::invalid_argument("Invalid fixture prefix");
    }
    if (mkdir(base, 0700) != 0 && errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), "Create fixture directory");
    }
    struct stat info {};
    if (lstat(base, &info) != 0) {
        throw std::system_error(errno, std::generic_category(), "Inspect fixture directory");
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0777) != 0700) {
        throw std::runtime_error("Fixture directory must be private, owned and not a symlink");
    }
    std::string pattern = std::string(base) + "/" + prefix + "-XXXXXX";
    if (!mkdtemp(pattern.data())) {
        throw std::system_error(errno, std::generic_category(), "Create fixture");
    }
    return pattern;
}

} // namespace vela::test
