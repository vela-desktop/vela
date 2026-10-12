// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What vela-audio's tests use inside the fixture (fixture.py): its control
// socket, fake apps (vela-audio-fake-stream) and outside tools (wpctl).

#include <QLocalSocket>
#include <QProcess>
#include <QStringList>
#include <QTest>

#include <memory>

namespace harness {

// The fixture's devices (pipewire.conf).
inline const QString speakers = QStringLiteral("vela-test-speakers");
inline const QString headphones = QStringLiteral("vela-test-headphones");
inline const QString surround = QStringLiteral("vela-test-surround");
inline const QString microphone = QStringLiteral("vela-test-microphone");

// WirePlumber writes its state about a second after a change (fixture.py).
constexpr int wireplumberSaveDelayMs = 1500;

// One command to the fixture; its answer ("ok [value]" or "error ...").
inline QString control(const QString& command)
{
    QLocalSocket socket;
    socket.connectToServer(qEnvironmentVariable("VELA_AUDIO_FIXTURE_CONTROL"));
    if (!socket.waitForConnected(2000)) {
        return QStringLiteral("error: no fixture (run the test through tests/audio/fixture.py)");
    }
    socket.write(command.toUtf8() + '\n');
    socket.flush();
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(30000)) {
            return QStringLiteral("error: no answer to ") + command;
        }
    }
    return QString::fromUtf8(socket.readLine()).trimmed();
}

// A program inside the fixture (wpctl, pw-cli); its standard output.
inline QString run(const QString& program, const QStringList& arguments)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(10000) || process.exitCode() != 0) {
        qWarning() << program << arguments << "failed:" << process.readAllStandardError();
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

// A fake app: one stream, playing or recording (fake-stream.c).
class FakeStream {
public:
    explicit FakeStream(const QStringList& arguments)
    {
        m_process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QObject::connect(&m_process, &QProcess::readyReadStandardOutput, [this] {
            while (m_process.canReadLine()) {
                const QList<QByteArray> words = m_process.readLine().trimmed().split(' ');
                if (words.value(0) == "ready") {
                    m_node = words.value(1).toUInt();
                } else if (words.value(0) == "state") {
                    m_state = QString::fromLatin1(words.value(1));
                } else if (words.value(0) == "peaks") {
                    QList<double> peaks;
                    for (qsizetype i = 1; i < words.size(); i++) {
                        peaks.append(words[i].toDouble());
                    }
                    m_peaks.append(peaks);
                }
            }
        });
        m_process.start(qEnvironmentVariable("VELA_AUDIO_FAKE_STREAM"), arguments);
    }
    ~FakeStream() { close(); }

    // Its node id, once it is streaming; 0 if it never gets there.
    quint32 waitStreaming()
    {
        static_cast<void>(QTest::qWaitFor([this] { return m_state == QLatin1String("streaming") || m_process.state() == QProcess::NotRunning; },
            5000));
        return m_state == QLatin1String("streaming") ? m_node : 0;
    }
    quint32 node() const { return m_node; }
    // A command: pause, resume, quit.
    void send(const QString& line) { m_process.write(line.toUtf8() + '\n'); }
    // Recording: each channel's peak, about every 100 ms, since the start
    // or the last clearPeaks().
    const QList<QList<double>>& peaks() const { return m_peaks; }
    void clearPeaks() { m_peaks.clear(); }

    // Ends it; its exit status (not 0: a crash or a sanitizer's report).
    int close()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.closeWriteChannel(); // end of input: it exits
            if (!m_process.waitForFinished(5000)) {
                m_process.kill();
                m_process.waitForFinished();
            }
        }
        return m_process.exitStatus() == QProcess::NormalExit ? m_process.exitCode() : -1;
    }

private:
    QProcess m_process;
    quint32 m_node = 0;
    QString m_state;
    QList<QList<double>> m_peaks;
};

// A fake sound card with profiles (fake-device.c).
class FakeDevice {
public:
    FakeDevice()
    {
        m_process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QObject::connect(&m_process, &QProcess::readyReadStandardOutput, [this] {
            while (m_process.canReadLine()) {
                const QList<QByteArray> words = m_process.readLine().trimmed().split(' ');
                if (words.value(0) == "ready") {
                    m_id = words.value(1).toUInt();
                } else if (words.value(0) == "profile") {
                    m_sets.append({ words.value(1).toInt(), words.value(2) == "1" });
                } else if (words.value(0) == "ignore") {
                    m_ignore = words.value(1) == "yes";
                }
            }
        });
        m_process.start(qEnvironmentVariable("VELA_AUDIO_FAKE_DEVICE"));
    }
    ~FakeDevice() { close(); }

    // Its device id once PipeWire has it; 0 if it never gets there.
    quint32 waitReady()
    {
        static_cast<void>(QTest::qWaitFor([this] { return m_id != 0 || m_process.state() == QProcess::NotRunning; }, 5000));
        return m_id;
    }
    // Every profile set on it: {index, to be saved}.
    const QList<QPair<int, bool>>& sets() const { return m_sets; }
    void send(const QString& line) { m_process.write(line.toUtf8() + '\n'); }
    bool ignoreProfileChanges(bool ignored)
    {
        send(ignored ? QStringLiteral("ignore yes") : QStringLiteral("ignore no"));
        return QTest::qWaitFor([this, ignored] { return m_ignore == ignored; }, 2000);
    }

    int close()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.closeWriteChannel();
            if (!m_process.waitForFinished(5000)) {
                m_process.kill();
                m_process.waitForFinished();
            }
        }
        return m_process.exitStatus() == QProcess::NormalExit ? m_process.exitCode() : -1;
    }

private:
    QProcess m_process;
    quint32 m_id = 0;
    QList<QPair<int, bool>> m_sets;
    bool m_ignore = false;
};

// A playing app with that program name (application.process.binary).
inline std::unique_ptr<FakeStream> player(const QString& binary, const QStringList& more = {})
{
    return std::make_unique<FakeStream>(QStringList { QStringLiteral("--prop"),
                                            QStringLiteral("application.process.binary=") + binary,
                                            QStringLiteral("--prop"), QStringLiteral("application.name=") + binary }
        + more);
}

// A recording app.
inline std::unique_ptr<FakeStream> recorder(const QString& binary, const QStringList& more = {})
{
    return player(binary, QStringList { QStringLiteral("--record"), QStringLiteral("--channels"), QStringLiteral("1") } + more);
}

} // namespace harness
