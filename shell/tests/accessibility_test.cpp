// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's accessibility and caffeine state (src/accessibility.h) against
// a fake compositor on a socket of its own: the state it reads, switches
// that move at once, and caffeine, which changes only once the compositor
// confirms it.

#include "accessibility.h"
#include "shellcontroller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutex>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <cmath>
#include <memory>

namespace {

// One-way, to the compositor.
QByteArrayList& commands()
{
    static QByteArrayList sent;
    return sent;
}

QByteArray json(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// The compositor's command socket, in a thread of its own: Accessibility
// blocks the test's thread while it waits for a reply.
class FakeCompositor {
public:
    explicit FakeCompositor(const QString& path)
        : m_path(path)
    {
        m_context.moveToThread(&m_thread);
        m_thread.start();
    }

    ~FakeCompositor()
    {
        stop();
        m_thread.quit();
        m_thread.wait();
    }

    // The reply to `command`; a command without one is read and left
    // unanswered.
    void reply(const QByteArray& command, const QByteArray& line)
    {
        QMutexLocker lock(&m_mutex);
        m_replies.insert(command, line);
    }

    QByteArrayList received() const
    {
        QMutexLocker lock(&m_mutex);
        return m_received;
    }

    bool start()
    {
        bool listening = false;
        QMetaObject::invokeMethod(
            &m_context,
            [&] {
                QLocalServer::removeServer(m_path);
                m_server = std::make_unique<QLocalServer>();
                listening = m_server->listen(m_path);
                QObject::connect(m_server.get(), &QLocalServer::newConnection, m_server.get(), [this] {
                    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
                        QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket] { answer(socket); });
                        QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                    }
                });
            },
            Qt::BlockingQueuedConnection);
        return listening;
    }

    void stop()
    {
        QMetaObject::invokeMethod(&m_context, [this] { m_server.reset(); }, Qt::BlockingQueuedConnection);
    }

private:
    void answer(QLocalSocket* socket)
    {
        while (socket->canReadLine()) {
            const QByteArray command = socket->readLine().trimmed();
            QMutexLocker lock(&m_mutex);
            m_received << command;
            if (const auto found = m_replies.constFind(command); found != m_replies.cend()) {
                socket->write(*found + '\n');
            }
        }
    }

    const QString m_path;
    QThread m_thread;
    QObject m_context; // lives in m_thread, like the server
    std::unique_ptr<QLocalServer> m_server;
    mutable QMutex m_mutex;
    QHash<QByteArray, QByteArray> m_replies;
    QByteArrayList m_received;
};

} // namespace

// Records instead of opening the compositor's socket.
bool ShellController::sendToCompositor(const QByteArray& command)
{
    commands() << command;
    return true;
}

class AccessibilityTest : public QObject {
    Q_OBJECT

    QTemporaryDir runtime;
    std::unique_ptr<FakeCompositor> compositor;

private slots:
    void initTestCase()
    {
        QVERIFY(runtime.isValid());
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
        qputenv("WAYLAND_DISPLAY", "test");
    }

    void init()
    {
        commands().clear();
        compositor = std::make_unique<FakeCompositor>(runtime.filePath(QStringLiteral("vela-test.sock")));
        QVERIFY(compositor->start());
    }

    void cleanup() { compositor.reset(); }

    void queryReadsTheState()
    {
        compositor->reply("accessibility",
            json({ { QStringLiteral("nightLight"), true }, { QStringLiteral("colorFilter"), true },
                { QStringLiteral("magnifier"), true }, { QStringLiteral("stickyKeys"), true },
                { QStringLiteral("caffeine"), true }, { QStringLiteral("caffeineSecs"), -1 },
                { QStringLiteral("caffeineDurationSecs"), 0 } }));
        Accessibility accessibility;
        QSignalSpy changed(&accessibility, &Accessibility::changed);
        accessibility.query();
        QCOMPARE(changed.size(), 1);
        QVERIFY(accessibility.nightLight());
        QVERIFY(accessibility.colorFilter());
        QVERIFY(accessibility.magnifier());
        QVERIFY(accessibility.stickyKeys());
        QVERIFY(accessibility.caffeine());
        QCOMPARE(accessibility.caffeineSecs(), -1);
        QCOMPARE(compositor->received(), QByteArrayList { "accessibility" });
    }

    void noStateWithoutAnAnswer()
    {
        Accessibility accessibility;
        QSignalSpy changed(&accessibility, &Accessibility::changed);
        accessibility.query(); // read, never answered
        compositor->stop();
        accessibility.query(); // nobody listening
        QVERIFY(changed.isEmpty());
        QVERIFY(!accessibility.nightLight());
    }

    void switchesMoveAtOnce()
    {
        Accessibility accessibility;
        QSignalSpy changed(&accessibility, &Accessibility::changed);
        accessibility.setNightLight(true);
        accessibility.setColorFilter(true);
        accessibility.setMagnifier(true);
        accessibility.setStickyKeys(true);
        QCOMPARE(changed.size(), 4);
        QVERIFY(accessibility.nightLight() && accessibility.colorFilter() && accessibility.magnifier()
            && accessibility.stickyKeys());
        accessibility.setNightLight(false);
        accessibility.setColorFilter(false);
        accessibility.setMagnifier(false);
        accessibility.setStickyKeys(false);
        QCOMPARE(commands(),
            (QByteArrayList { "night-light on", "color-filter on", "magnifier on", "sticky-keys on", "night-light off",
                "color-filter off", "magnifier off", "sticky-keys off" }));
        QVERIFY(!accessibility.nightLight() && !accessibility.colorFilter() && !accessibility.magnifier()
            && !accessibility.stickyKeys());
    }

    void caffeineOnceConfirmed()
    {
        compositor->reply("caffeine-set on",
            json({ { QStringLiteral("ok"), true },
                { QStringLiteral("state"),
                    QJsonObject { { QStringLiteral("caffeine"), true }, { QStringLiteral("caffeineSecs"), -1 } } } }));
        compositor->reply("caffeine-set off",
            json({ { QStringLiteral("ok"), true },
                { QStringLiteral("state"), QJsonObject { { QStringLiteral("caffeine"), false } } } }));
        Accessibility accessibility;
        accessibility.setCaffeine(true);
        QVERIFY(accessibility.caffeine());
        QCOMPARE(accessibility.caffeineSecs(), -1);
        accessibility.setCaffeine(false);
        QVERIFY(!accessibility.caffeine());
        QCOMPARE(accessibility.caffeineSecs(), 0);
    }

    void caffeineForAWhile()
    {
        compositor->reply("caffeine-set on 0.5",
            json({ { QStringLiteral("ok"), true },
                { QStringLiteral("state"),
                    QJsonObject { { QStringLiteral("caffeine"), true }, { QStringLiteral("caffeineSecs"), 30 },
                        { QStringLiteral("caffeineDurationSecs"), 30 } } } }));
        Accessibility accessibility;
        for (const double minutes : { 0.0, -1.0, 1441.0, std::nan(""), HUGE_VAL }) {
            accessibility.setCaffeineMinutes(minutes);
        }
        QVERIFY(compositor->received().isEmpty());
        accessibility.setCaffeineMinutes(0.5);
        QCOMPARE(compositor->received(), QByteArrayList { "caffeine-set on 0.5" });
        QVERIFY(accessibility.caffeine());
        QCOMPARE(accessibility.caffeineSecs(), 30);
        QCOMPARE(accessibility.caffeineDurationSecs(), 30);
    }

    void caffeineCountsDown()
    {
        Accessibility accessibility;
        accessibility.update(json({ { QStringLiteral("caffeine"), true }, { QStringLiteral("caffeineSecs"), 1 } }));
        QCOMPARE(accessibility.caffeineSecs(), 1);
        // Down to 0 on its own; the compositor then confirms the end.
        QTRY_COMPARE_WITH_TIMEOUT(accessibility.caffeineSecs(), 0, 3000);
        QVERIFY(accessibility.caffeine());
    }

    void caffeineRefused()
    {
        compositor->reply("caffeine-set on",
            json({ { QStringLiteral("ok"), false },
                { QStringLiteral("state"), QJsonObject { { QStringLiteral("caffeine"), false } } } }));
        Accessibility accessibility;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("rejected caffeine command")));
        accessibility.setCaffeine(true);
        QVERIFY(!accessibility.caffeine());
    }

    // Never shown before the compositor says so; asked again afterwards, in
    // case it applied the request and only the reply was lost.
    void caffeineUnconfirmed_data()
    {
        QTest::addColumn<bool>("listening");
        QTest::newRow("no reply") << true;
        QTest::newRow("no compositor") << false;
    }

    void caffeineUnconfirmed()
    {
        QFETCH(bool, listening);
        if (!listening) {
            compositor->stop();
        }
        Accessibility accessibility;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("caffeine state unconfirmed")));
        accessibility.setCaffeine(true);
        QVERIFY(!accessibility.caffeine());
        compositor->reply("accessibility", json({ { QStringLiteral("caffeine"), true } }));
        QVERIFY(compositor->start());
        QTRY_VERIFY_WITH_TIMEOUT(accessibility.caffeine(), 2000);
    }
};

QTEST_GUILESS_MAIN(AccessibilityTest)
#include "accessibility_test.moc"
