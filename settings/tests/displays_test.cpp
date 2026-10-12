// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "displays.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "vela-output-preview-v1-client-protocol.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstdio>
#include <csignal>
#include <memory>
#include <vector>
#include <wayland-client.h>

static QString serverExecutable;

// A separate ordinary output-management client exercises protocol ordering,
// including clients that abandon a protected request before applying it.
struct RawOutputClient {
    wl_display* display;
    wl_registry* registry;
    zwlr_output_manager_v1* manager = nullptr;
    vela_output_preview_manager_v1* previews = nullptr;
    zwlr_output_configuration_v1* configuration = nullptr;
    vela_output_preview_v1* preview = nullptr;
    std::vector<zwlr_output_head_v1*> heads;
    zwlr_output_head_v1* first = nullptr;
    std::vector<zwlr_output_mode_v1*> modes;
    uint32_t serial = 0;
    int result = -1, previewResult = -1;
    bool applied = false;

    template<class T> static void drop(T* proxy) { if (proxy) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(proxy)); }
    explicit RawOutputClient(const QString& socket) : display(wl_display_connect(socket.toUtf8().constData()))
    {
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener {
            [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
                auto* c = static_cast<RawOutputClient*>(data);
                if (QByteArray(interface) == zwlr_output_manager_v1_interface.name) {
                    c->manager = static_cast<zwlr_output_manager_v1*>(wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, 1));
                    static const zwlr_output_manager_v1_listener listener {
                        [](void* data, auto*, zwlr_output_head_v1* h) {
                            auto* c = static_cast<RawOutputClient*>(data);
                            c->heads.push_back(h);
                            static const zwlr_output_head_v1_listener listener {
                                .name = [](void* d, zwlr_output_head_v1* h, const char* name) {
                                    if (QByteArray(name) == "HEADLESS-1") static_cast<RawOutputClient*>(d)->first = h;
                                },
                                .description = [](void*, auto*, const char*) {},
                                .physical_size = [](void*, auto*, int32_t, int32_t) {},
                                .mode = [](void* data, auto*, zwlr_output_mode_v1* m) {
                                    auto* c = static_cast<RawOutputClient*>(data);
                                    c->modes.push_back(m);
                                    static const zwlr_output_mode_v1_listener listener {
                                        [](void*, auto*, int32_t, int32_t) {}, [](void*, auto*, int32_t) {},
                                        [](void*, auto*) {}, [](void*, auto*) {} };
                                    zwlr_output_mode_v1_add_listener(m, &listener, c);
                                },
                                .enabled = [](void*, auto*, int32_t) {},
                                .current_mode = [](void*, auto*, zwlr_output_mode_v1*) {},
                                .position = [](void*, auto*, int32_t, int32_t) {},
                                .transform = [](void*, auto*, int32_t) {},
                                .scale = [](void*, auto*, wl_fixed_t) {},
                                .finished = [](void*, auto*) {},
                                .make = nullptr, .model = nullptr, .serial_number = nullptr, .adaptive_sync = nullptr,
                            };
                            zwlr_output_head_v1_add_listener(h, &listener, c);
                        },
                        [](void* data, auto*, uint32_t serial) { static_cast<RawOutputClient*>(data)->serial = serial; },
                        [](void*, auto*) {},
                    };
                    zwlr_output_manager_v1_add_listener(c->manager, &listener, c);
                } else if (QByteArray(interface) == vela_output_preview_manager_v1_interface.name)
                    c->previews = static_cast<vela_output_preview_manager_v1*>(wl_registry_bind(registry, name, &vela_output_preview_manager_v1_interface, 1));
            }, [](void*, auto*, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, this);
        sync(); sync();
    }
    ~RawOutputClient()
    {
        if (preview) vela_output_preview_v1_destroy(preview);
        if (configuration) zwlr_output_configuration_v1_destroy(configuration);
        for (auto* m : modes) drop(m);
        for (auto* h : heads) drop(h);
        drop(previews); drop(manager); drop(registry);
        wl_display_disconnect(display);
    }
    bool sync() { return wl_display_roundtrip(display) >= 0; }
    void prepare(bool protect)
    {
        configuration = zwlr_output_manager_v1_create_configuration(manager, serial);
        static const zwlr_output_configuration_v1_listener listener {
            [](void* d, auto*) { static_cast<RawOutputClient*>(d)->result = 1; },
            [](void* d, auto*) { static_cast<RawOutputClient*>(d)->result = 0; },
            [](void* d, auto*) { static_cast<RawOutputClient*>(d)->result = 2; },
        };
        zwlr_output_configuration_v1_add_listener(configuration, &listener, this);
        for (size_t i = 0; i < heads.size(); ++i) {
            auto* wanted = zwlr_output_configuration_v1_enable_head(configuration, heads[i]);
            if (heads[i] == first) zwlr_output_configuration_head_v1_set_scale(wanted, wl_fixed_from_double(2));
            drop(wanted);
        }
        if (protect) {
            preview = vela_output_preview_manager_v1_protect(previews, wl_proxy_get_id(reinterpret_cast<wl_proxy*>(configuration)), "");
            static const vela_output_preview_v1_listener listener {
                [](void* d, auto*, uint32_t) { static_cast<RawOutputClient*>(d)->applied = true; },
                [](void* d, auto*, uint32_t result) { static_cast<RawOutputClient*>(d)->previewResult = result; },
            };
            vela_output_preview_v1_add_listener(preview, &listener, this);
        }
    }
};

class DisplaysTest : public QObject {
    Q_OBJECT
    QTemporaryDir runtime;
    QProcess server;
    QString socket;
    std::unique_ptr<Displays> client;

    void start(const QStringList& args = {})
    {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("XDG_RUNTIME_DIR", runtime.path());
        env.insert("XDG_CONFIG_HOME", runtime.path() + "/config");
        env.remove("WAYLAND_DISPLAY");
        server.setProcessEnvironment(env);
        server.start(serverExecutable, args);
        QVERIFY(server.waitForStarted());
        QVERIFY2(server.waitForReadyRead(), server.readAllStandardError().constData());
        socket = runtime.path() + '/' + QString::fromUtf8(server.readLine()).trimmed();
        client = connectClient();
        QVERIFY(client);
        QTRY_COMPARE_WITH_TIMEOUT(client->outputs().size(), 2, 2000);
    }
    std::unique_ptr<Displays> connectClient()
    {
        auto* display = wl_display_connect(socket.toUtf8().constData());
        if (!display) return {};
        return std::make_unique<Displays>(display, nullptr);
    }
    QVariantMap output(const QString& name) const
    {
        for (const auto& value : client->outputs()) if (value.toMap()["name"] == name) return value.toMap();
        return {};
    }
    QByteArray saved() const
    {
        QFile file(runtime.path() + "/config/vela/outputs.conf");
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
    void command(const QByteArray& text)
    {
        server.write(text + '\n');
        QVERIFY(server.waitForBytesWritten());
        QVERIFY(server.waitForReadyRead());
        QCOMPARE(server.readLine().trimmed(), QByteArray("ok"));
    }

private slots:
    void init() { QVERIFY(runtime.isValid()); }
    void cleanup()
    {
        client.reset();
        if (server.state() == QProcess::Running) {
            server.write("quit\n");
            QVERIFY(server.waitForFinished());
            QCOMPARE(server.exitStatus(), QProcess::NormalExit);
            QCOMPARE(server.exitCode(), 0);
        }
        QFile::remove(runtime.path() + "/config/vela/outputs.conf");
    }
    void nativeInventoryAndHotplug()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        QCOMPARE(output("HEADLESS-1")["current"].toMap()["refresh"].toDouble(), 60.0);
        QCOMPARE(output("HEADLESS-1")["modes"].toList().size(), 1);
        command("add");
        QTRY_COMPARE(client->outputs().size(), 3);
        command("remove HEADLESS-2");
        QTRY_COMPARE(client->outputs().size(), 2);
        QVERIFY(output("HEADLESS-2").isEmpty());
        QVERIFY(!output("HEADLESS-3").isEmpty());
    }
    void confirmPersistsOnlyAfterAcceptance()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        QSignalSpy applied(client.get(), &Displays::confirmationRequested);
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 1.5 }, { "x", -1280 }, { "y", -100 }, { "transform", "90" } }));
        QVERIFY(client->pending());
        QVERIFY(!client->apply("HEADLESS-1", { { "scale", 2 } }));
        QTRY_COMPARE(applied.count(), 1);
        QTRY_COMPARE(output("HEADLESS-2")["scale"].toDouble(), 1.5);
        QCOMPARE(output("HEADLESS-2")["x"].toInt(), -1280);
        QCOMPARE(output("HEADLESS-2")["y"].toInt(), -100);
        QCOMPARE(output("HEADLESS-2")["transform"].toString(), QString("90"));
        QVERIFY(saved().isEmpty());
        QVERIFY(client->secondsRemaining() > 0);
        client->confirm();
        QTRY_VERIFY(!client->pending());
        QVERIFY(saved().contains("scale=1.5"));
        QVERIFY(saved().contains("rotation=90"));
        const auto confirmed = saved();
        QTest::qWait(1400);
        QCOMPARE(output("HEADLESS-2")["scale"].toDouble(), 1.5);
        client.reset(); // Closing a confirmed client cannot undo the change.
        client = connectClient();
        QTRY_COMPARE(output("HEADLESS-2")["scale"].toDouble(), 1.5);
        QCOMPARE(saved(), confirmed);
    }
    void revertRestoresAllPropertiesAndSavedFile()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        const auto before = client->outputs();
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 1.25 } }));
        QTRY_VERIFY(client->confirming());
        client->confirm();
        QTRY_VERIFY(!client->pending());
        const auto confirmed = client->outputs();
        const auto disk = saved();
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 2 }, { "x", -960 }, { "y", -540 }, { "transform", "270" } }));
        QTRY_VERIFY(client->confirming());
        QCOMPARE(saved(), disk);
        client->revert();
        QTRY_VERIFY(!client->pending());
        QTRY_COMPARE(client->outputs(), confirmed);
        QCOMPARE(saved(), disk);
        QVERIFY(confirmed != before);
    }
    void timeoutDoesNotNeedTheClientEventLoop()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        const auto before = client->outputs();
        QVERIFY(client->apply("HEADLESS-1", { { "scale", 2 } }));
        QTRY_VERIFY(client->confirming());
        // Block the GUI thread beyond the deadline: only the compositor runs.
        QTest::qSleep(1400);
        client->confirm(); // Too late; cannot keep expired settings.
        QTRY_VERIFY(!client->pending());
        QTRY_COMPARE(client->outputs(), before);
        QVERIFY(saved().isEmpty());
    }
    void disconnectedClientRestoresImmediately()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        const auto before = client->outputs();
        auto other = connectClient();
        QTRY_VERIFY(other->available());
        QVERIFY(other->apply("HEADLESS-2", { { "enabled", false } }));
        QTRY_VERIFY(other->confirming());
        QTRY_VERIFY(!output("HEADLESS-2")["enabled"].toBool());
        other.reset();
        QTRY_COMPARE_WITH_TIMEOUT(client->outputs(), before, 500);
        QVERIFY(saved().isEmpty());
    }
    void killedSettingsBackendRestoresImmediately()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        const auto before = client->outputs();
        QProcess settings;
        settings.start(QCoreApplication::applicationFilePath(), { "--preview-client", socket });
        QVERIFY(settings.waitForStarted());
        QVERIFY(settings.waitForReadyRead());
        QCOMPARE(settings.readLine().trimmed(), QByteArray("applied"));
        QTRY_COMPARE(output("HEADLESS-1")["scale"].toDouble(), 1.5);
        settings.kill(); // SIGKILL: no client destructor or cleanup can run.
        QVERIFY(settings.waitForFinished());
        QCOMPARE(settings.exitStatus(), QProcess::CrashExit);
        QTRY_COMPARE_WITH_TIMEOUT(client->outputs(), before, 500);
        QVERIFY(saved().isEmpty());
    }
    void competingClientsCannotOverwriteAPreview()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        auto other = connectClient();
        QTRY_VERIFY(other->available());
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 1.5 } }));
        QTRY_VERIFY(client->confirming());
        QVERIFY(other->apply("HEADLESS-1", { { "scale", 2 } }));
        QTRY_VERIFY(!other->pending());
        QVERIFY(!other->error().isEmpty());
        QCOMPARE(output("HEADLESS-1")["scale"].toDouble(), 1.0);
        QVERIFY(client->confirming());
        client->revert();
        QTRY_VERIFY(!client->pending());
        QTRY_COMPARE(output("HEADLESS-2")["scale"].toDouble(), 1.0);
    }
    void reenableFromANewClientKeepsRememberedGeometry()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 2 }, { "x", -960 }, { "y", -540 }, { "transform", "90" } }));
        QTRY_VERIFY(client->confirming());
        client->confirm();
        QTRY_VERIFY(!client->pending());
        const auto enabled = output("HEADLESS-2");
        QVERIFY(client->apply("HEADLESS-2", { { "enabled", false } }));
        QTRY_VERIFY(client->confirming());
        client->confirm();
        QTRY_VERIFY(!client->pending());
        client.reset();
        client = connectClient();
        QTRY_VERIFY(client->available());
        QVERIFY(client->apply("HEADLESS-2", { { "enabled", true } }));
        QTRY_VERIFY(client->confirming());
        QTRY_COMPARE(output("HEADLESS-2"), enabled);
        client->revert();
        QTRY_VERIFY(!client->pending());
        QTRY_VERIFY(!output("HEADLESS-2")["enabled"].toBool());
        // An explicitly requested origin must override the remembered position.
        QVERIFY(client->apply("HEADLESS-2", { { "enabled", true }, { "x", 0 }, { "y", 0 } }));
        QTRY_VERIFY(client->confirming());
        QTRY_COMPARE(output("HEADLESS-2")["x"].toInt(), 0);
        QCOMPARE(output("HEADLESS-2")["y"].toInt(), 0);
    }
    void failedCommitRestoresPartialChanges()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        const auto before = client->outputs();
        command("fail HEADLESS-2");
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 2 }, { "x", -960 } }));
        QTRY_VERIFY(!client->pending());
        QVERIFY(!client->error().isEmpty());
        QTRY_COMPARE(client->outputs(), before);
        QVERIFY(saved().isEmpty());
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 1.5 } }));
        QTRY_VERIFY(client->confirming());
    }
    void hotplugDuringPreviewRestoresSurvivingOutputs()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        QVERIFY(client->apply("HEADLESS-1", { { "scale", 2 } }));
        QTRY_VERIFY(client->confirming());
        command("remove HEADLESS-2");
        command("add");
        QTRY_VERIFY(!output("HEADLESS-3").isEmpty());
        const auto added = output("HEADLESS-3");
        client->revert();
        QTRY_VERIFY(!client->pending());
        QTRY_COMPARE(output("HEADLESS-1")["scale"].toDouble(), 1.0);
        QCOMPARE(output("HEADLESS-3"), added);
    }
    void advertisedResolutionAndFrequency()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        QVERIFY(!client->apply("HEADLESS-1", { { "width", 999 }, { "height", 777 } }));
        QVERIFY(!client->apply("HEADLESS-1", { { "scale", 0 } }));
        QVERIFY(!client->apply("HEADLESS-1", { { "transform", "sideways" } }));
        QVERIFY(client->apply("HEADLESS-1", { { "width", 1920 }, { "height", 1080 }, { "refresh", 60.0 } }));
        QTRY_VERIFY(client->confirming());
        client->confirm();
        QTRY_VERIFY(!client->pending());
        QCOMPARE(output("HEADLESS-1")["current"].toMap()["refresh"].toDouble(), 60.0);
    }
    void unprotectedClientsKeepImmediateApplyBehavior()
    {
        start();
        QVERIFY(client);
        RawOutputClient other(socket);
        other.prepare(false);
        zwlr_output_configuration_v1_apply(other.configuration);
        QVERIFY(other.sync());
        QCOMPARE(other.result, 1);
        QTRY_COMPARE(output("HEADLESS-1")["scale"].toDouble(), 2.0);
        QVERIFY(saved().contains("scale=2"));
    }
    void unprotectedClientsCannotBypassActivePreview()
    {
        start();
        QVERIFY(client);
        QTRY_VERIFY(client->available());
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 1.5 } }));
        QTRY_VERIFY(client->confirming());
        RawOutputClient other(socket);
        other.prepare(false);
        zwlr_output_configuration_v1_apply(other.configuration);
        QVERIFY(other.sync());
        QCOMPARE(other.result, 0);
        QCOMPARE(output("HEADLESS-1")["scale"].toDouble(), 1.0);
        QVERIFY(saved().isEmpty());
    }
    void abandonedPreviewCannotBecomeAnUnprotectedApply()
    {
        start();
        QVERIFY(client);
        RawOutputClient other(socket);
        other.prepare(true);
        QVERIFY(other.sync());
        vela_output_preview_v1_destroy(other.preview);
        other.preview = nullptr;
        zwlr_output_configuration_v1_apply(other.configuration);
        QVERIFY(other.sync());
        QCOMPARE(other.result, 0);
        QCOMPARE(output("HEADLESS-1")["scale"].toDouble(), 1.0);
        QVERIFY(saved().isEmpty());
    }
    void preparedPreviewExpiresAndRejectsLateApply()
    {
        start();
        QVERIFY(client);
        RawOutputClient other(socket);
        other.prepare(true);
        QVERIFY(other.sync());
        QTest::qWait(1400);
        zwlr_output_configuration_v1_apply(other.configuration);
        QVERIFY(other.sync());
        QCOMPARE(other.previewResult, static_cast<int>(VELA_OUTPUT_PREVIEW_V1_RESULT_REVERTED));
        QCOMPARE(other.result, 0);
        QCOMPARE(output("HEADLESS-1")["scale"].toDouble(), 1.0);
        QVERIFY(saved().isEmpty());
    }
    void obsoleteConfigurationIsCancelledAndReleasesPreview()
    {
        start();
        QVERIFY(client);
        RawOutputClient other(socket);
        other.prepare(true);
        QVERIFY(other.sync());
        command("add");
        QTRY_VERIFY(!output("HEADLESS-3").isEmpty());
        zwlr_output_configuration_v1_apply(other.configuration);
        QVERIFY(other.sync());
        QCOMPARE(other.result, 2);
        zwlr_output_configuration_v1_destroy(other.configuration);
        other.configuration = nullptr;
        QVERIFY(other.sync());
        QVERIFY(client->apply("HEADLESS-2", { { "scale", 1.5 } }));
        QTRY_VERIFY(client->confirming());
    }
    void unsupportedCompositorCannotApplyUnsafeChanges()
    {
        start({ "legacy" });
        QVERIFY(client);
        QVERIFY(!client->available());
        QVERIFY(!client->apply("HEADLESS-1", { { "scale", 2 } }));
        QVERIFY(!client->pending());
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.value(1) == "--preview-client") {
        Displays client(wl_display_connect(args.value(2).toUtf8().constData()), nullptr);
        QTimer check;
        QObject::connect(&check, &QTimer::timeout, &client, [&] {
            if (client.available()) { check.stop(); client.apply("HEADLESS-1", { { "scale", 1.5 } }); }
        });
        QObject::connect(&client, &Displays::confirmationRequested, &client, [] { puts("applied"); fflush(stdout); });
        check.start(10);
        return app.exec();
    }
    serverExecutable = args.value(1);
    if (serverExecutable.isEmpty()) return 2;
    DisplaysTest tests;
    // Remove fixture path from QtTest's own arguments.
    char* testArgv[] = { argv[0], nullptr };
    return QTest::qExec(&tests, 1, testArgv);
}
#include "displays_test.moc"
