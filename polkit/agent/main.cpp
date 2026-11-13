// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-polkit-agent: Vela's polkit authentication agent
// (docs/polkit-agent.md). The compositor starts it as a child, so it is in
// the compositor's logind session; it registers with polkitd for that session
// and, for each request, starts vela-polkit-prompt. No Qt: GLib and
// libpolkit-agent-1.
//
// Exit codes: 0 closed with SIGTERM/SIGINT, 1 error, 2 another agent is
// already registered for this session (the compositor doesn't restart it).

#include "alloc/new_handler.h"
#include "agent.hpp"
#include "backend.hpp"
#include "system.hpp"

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>

#include <glib-unix.h>

#include <algorithm>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vela::polkit;

namespace {

constexpr const char* objectPath = "/org/vela/PolkitAgent";
constexpr int exitAlreadyRegistered = 2;

Agent* agent = nullptr;

// --------------------------------------------------- the GObject listener --
//
// initiate_authentication becomes Agent::begin; cancelling the request
// (a GCancellable, one per request) becomes Agent::cancel.

struct VelaListener {
    PolkitAgentListener parent;
};
struct VelaListenerClass {
    PolkitAgentListenerClass parent;
};

G_DEFINE_TYPE(VelaListener, vela_listener, POLKIT_AGENT_TYPE_LISTENER)

struct Pending {
    GTask* task;
    std::string cookie;
    GCancellable* cancellable;
    gulong cancelHandler = 0;
    bool finished = false;
};

std::vector<Identity> identitiesFrom(GList* identities)
{
    std::vector<Identity> result;
    for (GList* item = identities; item; item = item->next) {
        // polkitd already expands groups (unix-group:wheel) into users.
        if (!POLKIT_IS_UNIX_USER(item->data)) {
            continue;
        }
        const gint uid = polkit_unix_user_get_uid(POLKIT_UNIX_USER(item->data));
        if (auto identity = identityForUid(uint32_t(uid))) {
            result.push_back(*identity);
        }
    }
    return orderIdentities(std::move(result), uint32_t(getuid()));
}

Details detailsFrom(PolkitDetails* details)
{
    Details result;
    if (!details) {
        return result;
    }
    gchar** keys = polkit_details_get_keys(details);
    for (gchar** key = keys; key && *key; ++key) {
        const gchar* value = polkit_details_lookup(details, *key);
        result.emplace_back(*key, value ? value : "");
    }
    g_strfreev(keys);
    std::sort(result.begin(), result.end());
    return result;
}

void initiateAuthentication(PolkitAgentListener* listener, const gchar* actionId, const gchar* message,
    const gchar* iconName, PolkitDetails* details, const gchar* cookie, GList* identities,
    GCancellable* cancellable, GAsyncReadyCallback callback, gpointer userData)
{
    GTask* task = g_task_new(listener, cancellable, callback, userData);
    Request request;
    request.cookie = cookie ? cookie : "";
    request.actionId = actionId ? actionId : "";
    request.message = message ? message : "";
    request.iconName = iconName ? iconName : "";
    request.details = detailsFrom(details);
    request.identities = identitiesFrom(identities);
    const AppInfo app = describeCaller(request.details);
    request.appName = app.name;
    request.appIcon = app.icon;
    g_message("Request %s for %s (%zu identities)", request.actionId.c_str(),
        app.name.empty() ? "?" : app.name.c_str(), request.identities.size());

    auto* pending = new Pending { task, request.cookie, cancellable };
    if (cancellable) {
        g_object_ref(cancellable);
        // The signal can come from inside g_cancellable_cancel: the agent
        // hears it from the main loop. Only the cookie is passed: the request
        // may be finished (and freed) by then, and cancelling a cookie the
        // agent no longer has does nothing.
        pending->cancelHandler = g_cancellable_connect(cancellable,
            G_CALLBACK(+[](GCancellable*, gpointer data) {
                auto* pending = static_cast<Pending*>(data);
                if (pending->finished) {
                    return;
                }
                g_idle_add_full(
                    G_PRIORITY_DEFAULT,
                    [](gpointer data) -> gboolean {
                        if (agent) {
                            g_message("Request withdrawn by polkit");
                            agent->cancel(*static_cast<std::string*>(data));
                        }
                        return G_SOURCE_REMOVE;
                    },
                    new std::string(pending->cookie), [](gpointer data) { delete static_cast<std::string*>(data); });
            }),
            pending, nullptr);
    }

    agent->begin(std::move(request), [pending](Outcome outcome) {
        pending->finished = true;
        g_message("Request finished: %s",
            outcome == Outcome::Authorized ? "authorized" : outcome == Outcome::Cancelled ? "cancelled" : "failed");
        switch (outcome) {
        case Outcome::Authorized:
            g_task_return_boolean(pending->task, TRUE);
            break;
        case Outcome::Cancelled:
            g_task_return_new_error(pending->task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED, "Authentication cancelled");
            break;
        case Outcome::Failed:
            g_task_return_new_error(pending->task, POLKIT_ERROR, POLKIT_ERROR_FAILED, "Authentication dialog unavailable");
            break;
        }
        // Disconnect from the main loop: here we may be inside the "cancelled"
        // signal, and g_cancellable_disconnect would block there.
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* pending = static_cast<Pending*>(data);
                if (pending->cancellable) {
                    g_cancellable_disconnect(pending->cancellable, pending->cancelHandler);
                    g_object_unref(pending->cancellable);
                }
                g_object_unref(pending->task);
                delete pending;
                return G_SOURCE_REMOVE;
            },
            pending);
    });
}

gboolean initiateAuthenticationFinish(PolkitAgentListener*, GAsyncResult* result, GError** error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

void vela_listener_init(VelaListener*) { }

void vela_listener_class_init(VelaListenerClass* klass)
{
    auto* listener = POLKIT_AGENT_LISTENER_CLASS(klass);
    listener->initiate_authentication = initiateAuthentication;
    listener->initiate_authentication_finish = initiateAuthenticationFinish;
}

// ------------------------------------------------------- registration --

// The backend name the tests' fake polkitd reports (fake_polkitd.py); the real
// polkitd reports its JavaScript engine.
constexpr const char* fakeBackendName = "vela-fake-polkitd";

bool fakeAuthority()
{
    GError* error = nullptr;
    PolkitAuthority* authority = polkit_authority_get_sync(nullptr, &error);
    if (!authority) {
        g_clear_error(&error);
        return false;
    }
    const gchar* name = polkit_authority_get_backend_name(authority);
    const bool fake = name && std::strcmp(name, fakeBackendName) == 0;
    g_object_unref(authority);
    return fake;
}

struct Registration {
    PolkitAgentListener* listener = nullptr;
    PolkitSubject* subject = nullptr;
    gpointer handle = nullptr;
    bool polkitdSeen = false;
    int exitCode = 0;
    GMainLoop* loop = nullptr;
};

// The logind session the process runs in (the compositor's); if logind can't
// tell, XDG_SESSION_ID.
PolkitSubject* sessionSubject()
{
    GError* error = nullptr;
    PolkitSubject* subject = polkit_unix_session_new_for_process_sync(getpid(), nullptr, &error);
    if (subject) {
        return subject;
    }
    g_message("No logind session for this process (%s)", error ? error->message : "?");
    g_clear_error(&error);
    if (const char* id = std::getenv("XDG_SESSION_ID"); id && *id) {
        g_message("Using XDG_SESSION_ID=%s", id);
        return polkit_unix_session_new(id);
    }
    return nullptr;
}

bool registerAgent(Registration& registration)
{
    GError* error = nullptr;
    registration.handle = polkit_agent_listener_register(registration.listener, POLKIT_AGENT_REGISTER_FLAGS_NONE,
        registration.subject, objectPath, nullptr, &error);
    if (registration.handle) {
        gchar* subject = polkit_subject_to_string(registration.subject);
        g_message("Registered as the authentication agent for %s", subject);
        g_free(subject);
        return true;
    }
    const bool taken = error && std::strstr(error->message, "already exists");
    g_warning("Can't register with polkit: %s", error ? error->message : "?");
    g_clear_error(&error);
    registration.exitCode = taken ? exitAlreadyRegistered : 1;
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    vela::install_new_handler("vela-polkit-agent");
    if (argc > 1 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h"))) {
        std::printf("Usage: vela-polkit-agent\n"
                    "Vela's polkit authentication agent. Started by vela-compositor.\n"
                    "  VELA_POLKIT_PROMPT          the prompt to run instead of vela-polkit-prompt\n"
                    "  VELA_POLKIT_TEST_PASSWORD   tests only: no PAM, accept this password\n");
        return 0;
    }
    std::signal(SIGPIPE, SIG_IGN);
    // polkitd's messages (and app names) in Vela's language:
    // libpolkit-agent-1 sends polkitd the value of LANG.
    const std::string language = configuredLanguage();
    if (language == "it") {
        setenv("LANG", "it_IT.UTF-8", true);
    } else if (language == "en") {
        setenv("LANG", "en_US.UTF-8", true);
    }
    if (!language.empty()) {
        unsetenv("LANGUAGE");
        unsetenv("LC_ALL");
        unsetenv("LC_MESSAGES");
    }
    std::setlocale(LC_ALL, "");
    g_set_prgname("vela-polkit-agent");

    const std::string prompt = findPrompt();
    if (prompt.empty()) {
        g_warning("vela-polkit-prompt not found (VELA_POLKIT_PROMPT picks one)");
        return 1;
    }
    const char* testPassword = std::getenv("VELA_POLKIT_TEST_PASSWORD");
    if (testPassword && *testPassword) {
        // On the real polkitd a test agent would authorize nothing, but it
        // would take the session agent's place: only with the tests' fake
        // polkitd, on a private bus. The address alone proves nothing (it can
        // name the real system bus): the authority must say it is the fake.
        if (!std::getenv("DBUS_SYSTEM_BUS_ADDRESS") || !fakeAuthority()) {
            g_warning("VELA_POLKIT_TEST_PASSWORD works only with the tests' fake polkitd on a private "
                      "DBUS_SYSTEM_BUS_ADDRESS");
            return 1;
        }
        g_message("Test mode: no PAM, a fixed password");
    }
    GLibBackend backend(prompt, testPassword ? testPassword : "");
    Agent core(backend);
    agent = &core;

    Registration registration;
    registration.subject = sessionSubject();
    if (!registration.subject) {
        g_warning("Not in a login session: nothing to register for");
        return 1;
    }
    registration.listener = POLKIT_AGENT_LISTENER(g_object_new(vela_listener_get_type(), nullptr));
    if (!registerAgent(registration)) {
        agent = nullptr;
        g_object_unref(registration.listener);
        g_object_unref(registration.subject);
        return registration.exitCode;
    }
    registration.loop = g_main_loop_new(nullptr, FALSE);

    // polkitd restarted (a package update): registration must be redone,
    // libpolkit-agent-1 doesn't do it by itself.
    const guint watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.freedesktop.PolicyKit1", G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, gpointer data) {
            auto* registration = static_cast<Registration*>(data);
            if (!registration->polkitdSeen) {
                registration->polkitdSeen = true;
                return;
            }
            g_message("polkitd is back: registering again");
            if (registration->handle) {
                polkit_agent_listener_unregister(registration->handle);
                registration->handle = nullptr;
            }
            if (!registerAgent(*registration)) {
                g_main_loop_quit(registration->loop);
            }
        },
        [](GDBusConnection*, const gchar*, gpointer data) {
            auto* registration = static_cast<Registration*>(data);
            if (registration->polkitdSeen) {
                g_message("polkitd went away");
            }
            registration->polkitdSeen = true; // register again when it comes back
        },
        &registration, nullptr);

    auto quit = [](gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(data));
        return G_SOURCE_REMOVE;
    };
    g_unix_signal_add(SIGTERM, quit, registration.loop);
    g_unix_signal_add(SIGINT, quit, registration.loop);
    g_main_loop_run(registration.loop);

    g_bus_unwatch_name(watch);
    if (registration.handle) {
        polkit_agent_listener_unregister(registration.handle);
    }
    agent = nullptr;
    g_object_unref(registration.listener);
    g_object_unref(registration.subject);
    g_main_loop_unref(registration.loop);
    return registration.exitCode;
}
