// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's and Settings' brightness model (src/brightness.h,
// docs/brightness.md §9) against a fake compositor on a socket of its own:
// what a slider shows while the compositor catches up, refuses, restarts or
// never answers.

#include "brightness.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

struct State {
    QString name = QStringLiteral("DP-3");
    QString state = QStringLiteral("ready");
    double value = 0.8;
    bool pending = false;
    bool refused = false;
    int max = 100;
    quint64 seq = 0;
};

QByteArray line(const QList<State>& outputs)
{
    QJsonArray list;
    for (const State& s : outputs) {
        list.append(QJsonObject {
            { QStringLiteral("name"), s.name },
            { QStringLiteral("description"), s.name == QLatin1String("DP-3") ? QStringLiteral("Mi Monitor") : QString() },
            { QStringLiteral("mechanism"), QStringLiteral("ddc") },
            { QStringLiteral("state"), s.state },
            { QStringLiteral("value"), s.value < 0 ? QJsonValue() : QJsonValue(s.value) },
            { QStringLiteral("pending"), s.pending },
            { QStringLiteral("refused"), s.refused },
            { QStringLiteral("max"), s.max },
            { QStringLiteral("seq"), double(s.seq) },
        });
    }
    return QJsonDocument(QJsonObject { { QStringLiteral("outputs"), list } }).toJson(QJsonDocument::Compact) + '\n';
}

class BrightnessModel : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString path;
    QLocalServer server;
    QLocalSocket* compositor = nullptr;
    QByteArray received;

    void listen()
    {
        QVERIFY(server.listen(path));
    }
    // The model connected and asked to watch.
    void accept()
    {
        QTRY_VERIFY(server.hasPendingConnections() || compositor);
        if (!compositor) {
            QLocalSocket* socket = server.nextPendingConnection();
            compositor = socket;
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                const QByteArray bytes = socket->readAll();
                if (socket == compositor) {
                    received += bytes; // not a previous test's connection
                }
            });
            received += socket->readAll(); // what came before the connection above
        }
        QTRY_VERIFY(received.contains("watch brightness\n"));
    }
    void push(const QList<State>& outputs)
    {
        compositor->write(line(outputs));
        compositor->flush();
    }
    // What the model sent since the last clear (no side effects: QTRY_*
    // evaluates it more than once).
    QStringList lines() const { return QString::fromUtf8(received).split(u'\n', Qt::SkipEmptyParts); }
    void clear() { received.clear(); }

private slots:
    void init()
    {
        path = dir.filePath(QStringLiteral("vela-test.sock"));
        compositor = nullptr;
        received.clear();
        listen();
    }
    void cleanup()
    {
        server.close();
        QLocalServer::removeServer(path);
    }

    void outputsAndLabels()
    {
        Brightness model(nullptr, path);
        accept();
        State dp;
        State off { QStringLiteral("HDMI-A-1"), QStringLiteral("no-answer"), -1 };
        push({ dp, off });
        QTRY_COMPARE(model.outputs().size(), 2);
        const QVariantMap first = model.output(QStringLiteral("DP-3"));
        QCOMPARE(first.value(QStringLiteral("label")).toString(), QStringLiteral("Mi Monitor"));
        QCOMPARE(first.value(QStringLiteral("available")).toBool(), true);
        QCOMPARE(first.value(QStringLiteral("value")).toDouble(), 0.8);
        const QVariantMap second = model.output(QStringLiteral("HDMI-A-1"));
        QCOMPARE(second.value(QStringLiteral("label")).toString(), QStringLiteral("HDMI-A-1"));
        QCOMPARE(second.value(QStringLiteral("available")).toBool(), false);
        QCOMPARE(second.value(QStringLiteral("reason")).toString(), QStringLiteral("no-answer"));
        QVERIFY(model.output(QStringLiteral("nope")).isEmpty());
    }

    // The slider shows the request at once; a state the compositor sent
    // before taking it doesn't put it back.
    void anOlderStateDoesNotMoveTheSlider()
    {
        Brightness model(nullptr, path);
        accept();
        push({ State {} });
        QTRY_COMPARE(model.outputs().size(), 1);
        clear();
        model.setValue(QStringLiteral("DP-3"), 0.3);
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.3);
        QTRY_COMPARE(lines(), QStringList { QStringLiteral("brightness DP-3 0.3") });
        // A live read of the old value, from before the request.
        State stale;
        stale.value = 0.75;
        push({ stale });
        QTest::qWait(50);
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.3);
        // Taken: from now on the compositor's.
        State taken;
        taken.value = 0.3;
        taken.pending = true;
        taken.seq = 1;
        push({ taken });
        taken.pending = false;
        taken.value = 0.31; // the monitor confirms something else
        push({ taken });
        QTRY_COMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.31);
    }

    // During a drag the compositor's answers to earlier values never pull the
    // knob back.
    void aDrag()
    {
        Brightness model(nullptr, path);
        accept();
        push({ State {} });
        QTRY_COMPARE(model.outputs().size(), 1);
        for (double v : { 0.5, 0.4, 0.4, 0.3 }) {
            model.setValue(QStringLiteral("DP-3"), v);
        }
        State first;
        first.value = 0.5;
        first.pending = true;
        first.seq = 1;
        push({ first });
        QTest::qWait(50);
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.3);
        State last = first;
        last.value = 0.3;
        last.seq = 3; // 0.4 twice is one target
        push({ last });
        QTRY_VERIFY(model.output(QStringLiteral("DP-3")).value(QStringLiteral("pending")).toBool());
        last.pending = false;
        push({ last });
        QTRY_VERIFY(!model.output(QStringLiteral("DP-3")).value(QStringLiteral("pending")).toBool());
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.3);
    }

    void aRefusalPutsItBack()
    {
        Brightness model(nullptr, path);
        accept();
        push({ State {} });
        QTRY_COMPARE(model.outputs().size(), 1);
        model.setValue(QStringLiteral("DP-3"), 0.2);
        State refused;
        refused.refused = true;
        refused.seq = 1;
        push({ refused });
        QTRY_VERIFY(model.output(QStringLiteral("DP-3")).value(QStringLiteral("refused")).toBool());
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.8);
    }

    void aRequestNeverTakenTimesOut()
    {
        Brightness model(nullptr, path, 200);
        accept();
        push({ State {} });
        QTRY_COMPARE(model.outputs().size(), 1);
        model.setValue(QStringLiteral("DP-3"), 0.1);
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.1);
        QTRY_COMPARE_WITH_TIMEOUT(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.8,
            1000);
    }

    void nothingToSetWhenNotReady()
    {
        Brightness model(nullptr, path);
        accept();
        State locked;
        locked.state = QStringLiteral("not-responding");
        push({ locked });
        QTRY_COMPARE(model.outputs().size(), 1);
        const QVariantMap entry = model.output(QStringLiteral("DP-3"));
        QVERIFY(entry.value(QStringLiteral("available")).toBool());
        QVERIFY(entry.value(QStringLiteral("locked")).toBool());
        clear();
        model.setValue(QStringLiteral("DP-3"), 0.3);
        model.step(QStringLiteral("DP-3"), 1);
        QTest::qWait(50);
        QVERIFY(lines().isEmpty());
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.8);
    }

    void theWheelSteps()
    {
        Brightness model(nullptr, path);
        accept();
        push({ State {} });
        QTRY_COMPARE(model.outputs().size(), 1);
        clear();
        model.step(QStringLiteral("DP-3"), 1);
        model.step(QStringLiteral("DP-3"), 1);
        model.step(QStringLiteral("DP-3"), -1);
        QTRY_COMPARE(lines().size(), 3);
        clear();
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 0.82);
        for (int i = 0; i < 20; ++i) {
            model.step(QStringLiteral("DP-3"), 1);
        }
        QCOMPARE(model.output(QStringLiteral("DP-3")).value(QStringLiteral("value")).toDouble(), 1.0);
    }

    void liveAndReconnecting()
    {
        Brightness model(nullptr, path);
        accept();
        model.setLive(true);
        QTRY_VERIFY(received.contains("brightness-live on\n"));
        push({ State {} });
        QTRY_COMPARE(model.outputs().size(), 1);
        QSignalSpy changed(&model, &Brightness::changed);
        // The compositor restarts.
        compositor->disconnectFromServer();
        compositor->deleteLater();
        compositor = nullptr;
        received.clear();
        QTRY_VERIFY(model.outputs().isEmpty());
        QVERIFY(!model.connected());
        accept(); // within a second it is back, and asks again
        QTRY_VERIFY(received.contains("brightness-live on\n"));
        QVERIFY(model.connected());
        model.setLive(false);
        QTRY_VERIFY(received.contains("brightness-live off\n"));
    }

    void probe()
    {
        Brightness model(nullptr, path);
        accept();
        clear();
        model.probe(QStringLiteral("HDMI-A-1"));
        QTRY_COMPARE(lines(), QStringList { QStringLiteral("brightness-probe HDMI-A-1") });
    }
};

} // namespace

QTEST_GUILESS_MAIN(BrightnessModel)
#include "brightness_model.moc"
