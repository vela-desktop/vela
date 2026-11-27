// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "backend.hpp"

#include "../protocol.hpp"

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>

#include <glib-unix.h>
#include <glib.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vela::polkit {

namespace {

// ----------------------------------------------------- real session --

// A conversation with polkit-agent-helper-1 (through the socket, or the
// setuid helper): libpolkit-agent-1 takes care of it.
class PolkitSession : public AuthSession {
public:
    PolkitSession(const Identity& identity, const std::string& cookie, Events events)
        : m_events(std::move(events))
    {
        PolkitIdentity* user = polkit_unix_user_new(gint(identity.uid));
        m_session = polkit_agent_session_new(user, cookie.c_str());
        g_object_unref(user);
        g_signal_connect(m_session, "request", G_CALLBACK(onRequest), this);
        g_signal_connect(m_session, "show-error", G_CALLBACK(onError), this);
        g_signal_connect(m_session, "show-info", G_CALLBACK(onInfo), this);
        g_signal_connect(m_session, "completed", G_CALLBACK(onCompleted), this);
        // From the main loop: if the helper doesn't start, "completed" comes
        // at once, and it must not come while the agent is still creating us.
        m_idle = g_idle_add(
            [](gpointer data) -> gboolean {
                auto* self = static_cast<PolkitSession*>(data);
                self->m_idle = 0;
                polkit_agent_session_initiate(self->m_session);
                return G_SOURCE_REMOVE;
            },
            this);
    }

    ~PolkitSession() override
    {
        cancel();
        g_object_unref(m_session);
    }

    void respond(std::string& answer) override
    {
        if (!m_done) {
            polkit_agent_session_response(m_session, answer.c_str());
        }
        wipe(answer);
    }

    void cancel() override
    {
        if (m_idle) {
            g_source_remove(m_idle);
            m_idle = 0;
            m_done = true;
        }
        // polkit_agent_session_cancel emits "completed" right away: whoever
        // cancels doesn't want to hear it.
        g_signal_handlers_disconnect_by_data(m_session, this);
        if (!m_done) {
            m_done = true;
            polkit_agent_session_cancel(m_session);
        }
    }

private:
    static void onRequest(PolkitAgentSession*, const gchar* text, gboolean echo, gpointer data)
    {
        static_cast<PolkitSession*>(data)->m_events.request(text ? text : "", echo);
    }
    static void onError(PolkitAgentSession*, const gchar* text, gpointer data)
    {
        static_cast<PolkitSession*>(data)->m_events.error(text ? text : "");
    }
    static void onInfo(PolkitAgentSession*, const gchar* text, gpointer data)
    {
        static_cast<PolkitSession*>(data)->m_events.info(text ? text : "");
    }
    static void onCompleted(PolkitAgentSession*, gboolean authorized, gpointer data)
    {
        auto* self = static_cast<PolkitSession*>(data);
        self->m_done = true;
        self->m_events.completed(authorized);
    }

    Events m_events;
    PolkitAgentSession* m_session = nullptr;
    guint m_idle = 0;
    bool m_done = false;
};

// ----------------------------------------------------- test session --

// For tests: asks "Password:" and accepts only the test password, after a
// moment (like PAM). Talks to neither the helper nor PAM.
class TestSession : public AuthSession {
public:
    TestSession(std::string password, Events events)
        : m_password(std::move(password))
        , m_events(std::move(events))
    {
        m_timer = g_idle_add(
            [](gpointer data) -> gboolean {
                auto* self = static_cast<TestSession*>(data);
                self->m_timer = 0;
                self->m_events.request("Password: ", false);
                return G_SOURCE_REMOVE;
            },
            this);
    }
    ~TestSession() override { cancel(); }

    void respond(std::string& answer) override
    {
        if (m_timer == 0) {
            m_right = answer == m_password;
            m_timer = g_timeout_add(300,
                [](gpointer data) -> gboolean {
                    auto* self = static_cast<TestSession*>(data);
                    self->m_timer = 0;
                    self->m_events.completed(self->m_right);
                    return G_SOURCE_REMOVE;
                },
                this);
        }
        wipe(answer);
    }

    void cancel() override
    {
        if (m_timer) {
            g_source_remove(m_timer);
            m_timer = 0;
        }
    }

private:
    std::string m_password;
    Events m_events;
    guint m_timer = 0;
    bool m_right = false;
};

// ---------------------------------------------------------- prompt --

class ProcessPrompt : public Prompt {
public:
    static std::unique_ptr<ProcessPrompt> start(const std::string& path, Events events)
    {
        gchar* argv[] = { const_cast<gchar*>(path.c_str()), nullptr };
        GPid pid = 0;
        gint in = -1;
        gint out = -1;
        GError* error = nullptr;
        // stderr inherited: it ends up in the session log, with ours.
        if (!g_spawn_async_with_pipes(nullptr, argv, nullptr,
                GSpawnFlags(G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CLOEXEC_PIPES), nullptr, nullptr, &pid, &in, &out,
                nullptr, &error)) {
            g_warning("Can't start %s: %s", path.c_str(), error->message);
            g_error_free(error);
            return nullptr;
        }
        return std::unique_ptr<ProcessPrompt>(new ProcessPrompt(pid, in, out, std::move(events)));
    }

    ~ProcessPrompt() override
    {
        // With stdin closed, a prompt that hasn't finished yet closes by itself.
        // The child watch may still come: it must not find us.
        if (m_link) {
            m_link->owner = nullptr;
        }
        if (m_in >= 0) {
            close(m_in);
        }
        if (m_watch) {
            g_source_remove(m_watch);
        }
        close(m_out);
        m_lines.wipe();
    }

    void send(const std::string& line) override
    {
        size_t written = 0;
        while (m_in >= 0 && written < line.size()) {
            const ssize_t n = write(m_in, line.data() + written, line.size() - written);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                close(m_in); // the prompt is gone: its exit will tell
                m_in = -1;
                break;
            }
            written += size_t(n);
        }
    }

private:
    // The child must be reaped even after the prompt is destroyed. The watch
    // owns the link and frees it; whichever of the two goes first unhooks
    // itself from the other.
    struct Link {
        ProcessPrompt* owner;
    };

    // It owns descriptors and a GLib watch: never copied.
    ProcessPrompt(const ProcessPrompt&) = delete;
    ProcessPrompt& operator=(const ProcessPrompt&) = delete;

    ProcessPrompt(GPid pid, int in, int out, Events events)
        : m_in(in)
        , m_out(out)
        , m_events(std::move(events))
        , m_link(new Link { this })
    {
        g_unix_set_fd_nonblocking(m_out, TRUE, nullptr);
        m_watch = g_unix_fd_add(m_out, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), onReadable, this);
        g_child_watch_add(pid,
            [](GPid pid, gint, gpointer data) {
                auto* link = static_cast<Link*>(data);
                g_spawn_close_pid(pid);
                if (ProcessPrompt* owner = link->owner) {
                    owner->m_link = nullptr;
                    owner->exited();
                }
                delete link;
            },
            m_link);
    }

    static gboolean onReadable(gint fd, GIOCondition, gpointer data)
    {
        auto* self = static_cast<ProcessPrompt*>(data);
        char buffer[4096];
        for (;;) {
            const ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                self->m_lines.append(std::string_view(buffer, size_t(n)));
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            // End of stdout: for us the prompt is done.
            self->m_watch = 0;
            self->deliver();
            self->exited();
            return G_SOURCE_REMOVE;
        }
        std::fill(std::begin(buffer), std::end(buffer), '\0');
        self->deliver();
        if (self->m_lines.pendingSize() > 64 * 1024) {
            g_warning("vela-polkit-prompt: line too long, closing it");
            self->m_watch = 0;
            self->exited();
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    void deliver()
    {
        std::string line;
        while (!m_exited && m_lines.next(line)) {
            m_events.line(line);
            wipe(line);
        }
    }

    void exited()
    {
        if (!m_exited) {
            m_exited = true;
            m_events.exited();
        }
    }

    int m_in;
    int m_out;
    Events m_events;
    Link* m_link;
    guint m_watch = 0;
    bool m_exited = false;
    LineBuffer m_lines;
};

} // namespace

GLibBackend::GLibBackend(std::string promptPath, std::string testPassword)
    : m_promptPath(std::move(promptPath))
    , m_testPassword(std::move(testPassword))
{
}

std::unique_ptr<AuthSession> GLibBackend::startSession(
    const Identity& identity, const std::string& cookie, AuthSession::Events events)
{
    if (!m_testPassword.empty()) {
        return std::make_unique<TestSession>(m_testPassword, std::move(events));
    }
    return std::make_unique<PolkitSession>(identity, cookie, std::move(events));
}

std::unique_ptr<Prompt> GLibBackend::startPrompt(Prompt::Events events)
{
    return ProcessPrompt::start(m_promptPath, std::move(events));
}

void GLibBackend::defer(std::function<void()> task)
{
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(task)),
        [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

std::string findPrompt()
{
    if (const char* custom = std::getenv("VELA_POLKIT_PROMPT"); custom && *custom) {
        return custom;
    }
    char self[PATH_MAX] {};
    if (const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1); n > 0) {
        std::string dir(self, size_t(n));
        dir.resize(dir.rfind('/'));
        if (access((dir + "/vela-polkit-prompt").c_str(), X_OK) == 0) {
            return dir + "/vela-polkit-prompt";
        }
    }
    const std::string installed = std::string(VELA_LIBEXECDIR) + "/vela-polkit-prompt";
    return access(installed.c_str(), X_OK) == 0 ? installed : std::string();
}

} // namespace vela::polkit
