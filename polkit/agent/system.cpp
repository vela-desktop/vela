// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system.hpp"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <climits>
#include <fstream>
#include <pwd.h>
#include <unistd.h>
#include <vector>

namespace vela::polkit {

namespace {

std::string basename(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool readable(const std::string& path)
{
    return !path.empty() && access(path.c_str(), R_OK) == 0;
}

// The process's argv, from /proc/<pid>/cmdline.
std::vector<std::string> argumentsOf(int pid)
{
    std::ifstream file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::vector<std::string> arguments;
    std::string argument;
    while (std::getline(file, argument, '\0')) {
        arguments.push_back(argument);
    }
    return arguments;
}

// The installed app whose Exec runs `program` (path or name).
std::optional<AppInfo> installedApp(const std::string& program)
{
    if (program.empty()) {
        return std::nullopt;
    }
    const std::string wanted = basename(program);
    // Shells and interpreters run anything (Exec=sh -c ...): they don't tell
    // which app it is.
    for (const char* generic : { "sh", "bash", "zsh", "fish", "dash", "env", "python", "python3", "perl", "ruby",
             "node", "flatpak", "gjs" }) {
        if (wanted == generic || wanted.rfind(std::string(generic) + ".", 0) == 0) {
            return std::nullopt;
        }
    }
    std::optional<AppInfo> found;
    GList* apps = g_app_info_get_all();
    for (GList* item = apps; item && !found; item = item->next) {
        GAppInfo* app = G_APP_INFO(item->data);
        const char* executable = g_app_info_get_executable(app);
        if (!executable || basename(executable) != wanted) {
            continue;
        }
        AppInfo info;
        info.name = g_app_info_get_display_name(app) ? g_app_info_get_display_name(app) : wanted;
        if (GIcon* icon = g_app_info_get_icon(app)) {
            if (G_IS_THEMED_ICON(icon)) {
                const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
                info.icon = names && names[0] ? names[0] : "";
            } else if (G_IS_FILE_ICON(icon)) {
                gchar* path = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)));
                info.icon = path ? path : "";
                g_free(path);
            }
        }
        found = info;
    }
    g_list_free_full(apps, g_object_unref);
    return found;
}

std::string detail(const Details& details, const char* key)
{
    for (const auto& [name, value] : details) {
        if (name == key) {
            return value;
        }
    }
    return {};
}

} // namespace

std::optional<Identity> identityForUid(uint32_t uid)
{
    std::vector<char> buffer(16384);
    passwd entry {};
    passwd* result = nullptr;
    if (getpwuid_r(uid_t(uid), &entry, buffer.data(), buffer.size(), &result) != 0 || !result) {
        return std::nullopt;
    }
    Identity identity;
    identity.uid = uid;
    identity.login = entry.pw_name ? entry.pw_name : std::to_string(uid);
    // GECOS: "Full Name,room,phone...": the first field counts.
    std::string gecos = entry.pw_gecos ? entry.pw_gecos : "";
    gecos = gecos.substr(0, gecos.find(','));
    identity.name = gecos.empty() ? identity.login : gecos;
    // Like vela-lock: AccountsService's picture, then ~/.face.icon and ~/.face.
    const std::string home = entry.pw_dir ? entry.pw_dir : "";
    for (const std::string& path : { "/var/lib/AccountsService/icons/" + identity.login, home + "/.face.icon",
             home + "/.face" }) {
        if (!home.empty() && readable(path)) {
            identity.avatar = path;
            break;
        }
    }
    return identity;
}

std::string executableOf(int pid)
{
    char target[PATH_MAX] {};
    const std::string link = "/proc/" + std::to_string(pid) + "/exe";
    const ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
    return n > 0 ? std::string(target, size_t(n)) : std::string();
}

AppInfo describeCaller(const Details& details)
{
    // pkexec: the program to run counts, not whoever started pkexec.
    if (const std::string program = detail(details, "program"); !program.empty()) {
        if (auto app = installedApp(program)) {
            return *app;
        }
        return { basename(program), {} };
    }
    const std::string exe = callerExecutable(details, executableOf);
    if (exe.empty()) {
        return {};
    }
    if (auto app = installedApp(exe)) {
        return *app;
    }
    // The name it was started with (run0 is a link to systemd-run) and, for
    // interpreters, the script.
    int pid = 0;
    for (const char* key : { "polkit.subject-pid", "polkit.caller-pid" }) {
        if (const std::string value = detail(details, key); !value.empty()) {
            pid = std::atoi(value.c_str());
            if (executableOf(pid) == exe) {
                break;
            }
        }
    }
    const std::vector<std::string> arguments = pid > 0 ? argumentsOf(pid) : std::vector<std::string>();
    for (size_t i = 0; i < arguments.size() && i < 3; ++i) {
        if (i > 0 && (arguments[i].empty() || arguments[i][0] == '-')) {
            continue;
        }
        if (auto app = installedApp(arguments[i])) {
            return *app;
        }
    }
    return { arguments.empty() ? basename(exe) : basename(arguments[0]), {} };
}

std::string configuredLanguage()
{
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const std::string path = config && *config ? std::string(config) + "/vela/vela.conf"
        : home                                ? std::string(home) + "/.config/vela/vela.conf"
                                              : std::string();
    std::ifstream file(path);
    std::string line;
    std::string value;
    while (std::getline(file, line)) {
        // "lingua=" is the old name (shell/src/language.h).
        for (const std::string key : { "language=", "lingua=" }) {
            if (line.rfind(key, 0) == 0) {
                value = line.substr(key.size());
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t\r") + 1);
            }
        }
    }
    return value == "it" || value == "en" || value == "pt" ? value : std::string();
}

} // namespace vela::polkit
