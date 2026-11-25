// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "request.h"

#include <QVariantMap>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using namespace vela::polkit;

Request* Request::instance = nullptr;

namespace {

// The real stdout, where we write to the agent. Descriptor 1 goes to stderr: a
// library that prints something must not break the protocol.
int output = -1;

QString text(const std::string& value)
{
    return QString::fromStdString(value);
}

} // namespace

Request::Request(QObject* parent)
    : QObject(parent)
{
    instance = this;
}

Request* Request::create(QQmlEngine*, QJSEngine* engine)
{
    engine->setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}

void Request::listen()
{
    output = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    m_notifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Request::onReadable);
}

int Request::currentUid() const
{
    return int(getuid());
}

bool Request::chooseIdentity() const
{
    if (m_identities.size() < 2) {
        return false;
    }
    const int me = int(getuid());
    for (const QVariant& identity : m_identities) {
        if (identity.toMap().value(QStringLiteral("uid")).toInt() == me) {
            return false;
        }
    }
    return true;
}

void Request::onReadable()
{
    char buffer[4096];
    for (;;) {
        const ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n > 0) {
            m_buffer.append(std::string_view(buffer, size_t(n)));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        // The agent closed: for us the request is over.
        m_notifier->setEnabled(false);
        std::string line;
        while (m_buffer.next(line)) {
            if (auto message = parse(line)) {
                handle(message->word, message->argument);
            }
        }
        close();
        return;
    }
    std::string line;
    while (m_buffer.next(line)) {
        if (auto message = parse(line)) {
            handle(message->word, message->argument);
        }
    }
}

void Request::handle(const std::string& word, const std::string& argument)
{
    if (m_closing) {
        return;
    }
    if (word == "action") {
        m_actionId = text(argument);
    } else if (word == "message") {
        m_message = text(argument);
    } else if (word == "icon") {
        m_iconName = text(argument);
    } else if (word == "app") {
        m_appName = text(argument);
    } else if (word == "app-icon") {
        m_appIcon = text(argument);
    } else if (word == "detail") {
        const size_t equals = argument.find('=');
        m_details.append(QVariantMap {
            { QStringLiteral("key"), text(argument.substr(0, equals)) },
            { QStringLiteral("value"), equals == std::string::npos ? QString() : text(argument.substr(equals + 1)) },
        });
    } else if (word == "identity") {
        // "<uid> <login> <display name>"
        const size_t first = argument.find(' ');
        const size_t second = first == std::string::npos ? first : argument.find(' ', first + 1);
        const int uid = std::atoi(argument.substr(0, first).c_str());
        const QString login = first == std::string::npos ? QString() : text(argument.substr(first + 1, second - first - 1));
        const QString name = second == std::string::npos ? login : text(argument.substr(second + 1));
        m_identities.append(QVariantMap {
            { QStringLiteral("uid"), uid },
            { QStringLiteral("login"), login },
            { QStringLiteral("name"), name.isEmpty() ? login : name },
            { QStringLiteral("avatar"), QString() },
        });
        if (m_selectedUid < 0) {
            m_selectedUid = uid;
            emit selectedUidChanged();
        }
    } else if (word == "avatar") {
        const size_t space = argument.find(' ');
        const int uid = std::atoi(argument.substr(0, space).c_str());
        for (QVariant& identity : m_identities) {
            QVariantMap map = identity.toMap();
            if (map.value(QStringLiteral("uid")).toInt() == uid && space != std::string::npos) {
                map[QStringLiteral("avatar")] = text(argument.substr(space + 1));
                identity = map;
            }
        }
    } else if (word == "show") {
        emit changed();
        m_shown = true;
        emit shownChanged();
        return;
    } else if (word == "request") {
        // "<0|1> <text>"
        m_echo = argument.rfind("1", 0) == 0;
        m_promptText = text(argument.size() > 2 ? argument.substr(2) : std::string());
        m_asking = true;
        m_checking = false;
        emit conversationChanged();
        emit asked();
        return;
    } else if (word == "info") {
        m_infoText = text(argument);
        emit conversationChanged();
        return;
    } else if (word == "error") {
        m_errorText = text(argument);
        m_errorSinceResponse = true;
        emit conversationChanged();
        return;
    } else if (word == "retry") {
        // PAM usually doesn't say why: if it said nothing, it's the password.
        if (!m_errorSinceResponse) {
            m_errorText = tr("The password is incorrect. Try again.");
        }
        m_checking = false;
        m_asking = false; // the next question comes with the new session
        emit conversationChanged();
        emit retried();
        return;
    } else if (word == "failed") {
        m_unavailable = true;
        m_asking = false;
        m_checking = false;
        if (!m_errorSinceResponse) {
            m_errorText = tr("Authentication isn't available right now.");
        }
        emit conversationChanged();
        return;
    } else if (word == "done" || word == "cancel") {
        close();
        return;
    } else {
        return; // unknown word: from a newer agent
    }
    if (m_shown) {
        emit changed();
    }
}

void Request::respond(const QString& answer)
{
    if (!m_asking || m_checking || m_closing) {
        return;
    }
    QByteArray utf8 = answer.toUtf8();
    std::string line = encode("response", std::string_view(utf8.constData(), size_t(utf8.size())));
    utf8.fill('\0');
    write(line);
    wipe(line);
    m_checking = true;
    m_errorSinceResponse = false;
    m_errorText.clear();
    m_infoText.clear();
    emit conversationChanged();
}

void Request::selectIdentity(int uid)
{
    if (uid == m_selectedUid || m_closing) {
        return;
    }
    m_selectedUid = uid;
    emit selectedUidChanged();
    write(encode("identity", std::to_string(uid)));
    m_asking = false;
    m_checking = false;
    m_unavailable = false;
    m_errorText.clear();
    m_infoText.clear();
    m_errorSinceResponse = false;
    emit conversationChanged();
}

void Request::cancel()
{
    if (m_closing) {
        return;
    }
    write(encode("cancel"));
    close();
}

void Request::write(const std::string& line)
{
    size_t written = 0;
    while (output >= 0 && written < line.size()) {
        const ssize_t n = ::write(output, line.data() + written, line.size() - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return; // the agent is gone: stdin will notice
        }
        written += size_t(n);
    }
}

void Request::close()
{
    if (m_closing) {
        return;
    }
    m_closing = true;
    m_buffer.wipe();
    emit closingChanged();
}

void Request::setCapsLock(bool on)
{
    if (on != m_capsLock) {
        m_capsLock = on;
        emit capsLockChanged();
    }
}
