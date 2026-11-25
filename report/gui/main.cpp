// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
// A normal desktop window: no dependency on Vela's compositor or layer-shell.
#include "alloc/new_handler.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <csignal>
#include <unistd.h>

static volatile std::sig_atomic_t interrupted = 0;

static const QString privacy = QStringLiteral(
    "Text redaction is best effort. Review logs and descriptions before sharing. "
    "Images, videos, dumps and vendor archives are not automatically redacted. "
    "Nothing is uploaded; no settings are changed.");

static QLabel *paragraph(const QString &text)
{
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

class Reporter : public QMainWindow {
public:
    Reporter(const QString &python, const QJsonObject &options, const QString &smokeOutput = {})
        : options(options), smokeOutput(smokeOutput)
    {
        setWindowTitle(QStringLiteral("Vela Report"));
        resize(900, 760);
        auto *central = new QWidget;
        setCentralWidget(central);
        auto *layout = new QVBoxLayout(central);
        layout->addWidget(paragraph(QStringLiteral(
            "Create a local diagnostic ZIP to share with the Vela maintainer. "
            "Choose what to include, collect evidence, then review it before export.")));
        tabs = new QTabWidget;
        layout->addWidget(tabs);
        makeDescription();
        makeEvidence();
        makeReview();
        status = paragraph(QStringLiteral("Discovering available evidence…"));
        layout->addWidget(status);
        progress = new QProgressBar;
        progress->setRange(0, 0);
        progress->hide();
        layout->addWidget(progress);
        auto *buttons = new QHBoxLayout;
        stop = new QPushButton(QStringLiteral("Stop current collection"));
        stop->hide();
        connect(stop, &QPushButton::clicked, this, [this] {
            if (worker.state() == QProcess::Running)
                ::kill(static_cast<pid_t>(worker.processId()), SIGINT);
        });
        buttons->addWidget(stop);
        buttons->addStretch();
        auto *close = new QPushButton(QStringLiteral("Close"));
        connect(close, &QPushButton::clicked, this, &QWidget::close);
        buttons->addWidget(close);
        layout->addLayout(buttons);

        connect(&worker, &QProcess::readyReadStandardOutput, this, [this] {
            buffer += worker.readAllStandardOutput();
            while (buffer.contains('\n')) {
                const int end = buffer.indexOf('\n');
                const auto line = buffer.left(end);
                buffer.remove(0, end + 1);
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(line, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    fail(QStringLiteral("The reporting worker returned an invalid response."));
                    continue;
                }
                receive(document.object());
            }
        });
        connect(&worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                fail(QStringLiteral("Cannot start Python. Run vela-report --cli to check the installation."));
        });
        connect(&worker, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            if (!closing && !exported) {
                fail(QStringLiteral("The collection worker exited (%1). Temporary evidence was removed.").arg(code));
                tabs->setEnabled(false);
            }
        });
        worker.start(python, {QStringLiteral("-m"), QStringLiteral("vela_report"), QStringLiteral("--worker")});
        request({{"action", "discover"}, {"state_dir", options.value("state_dir")}});
    }

    ~Reporter() override
    {
        closing = true;
        if (worker.state() != QProcess::NotRunning) {
            if (!busy)
                worker.write("{\"action\":\"close\"}\n");
            else
                worker.terminate();
            if (!worker.waitForFinished(2500)) {
                worker.kill();
                worker.waitForFinished(1000);
            }
        }
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (!exported && smokeOutput.isEmpty() &&
            QMessageBox::question(this, "Cancel report",
                "Close without exporting? Temporary collected data will be removed.",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        event->accept();
        QCoreApplication::exit(exported ? 0 : 130);
    }

private:
    QProcess worker;
    QByteArray buffer;
    QJsonObject options, discovered, summary;
    QString smokeOutput;
    bool busy = false, closing = false, exported = false;
    QTabWidget *tabs = nullptr;
    QLabel *status = nullptr;
    QProgressBar *progress = nullptr;
    QPushButton *stop = nullptr;
    QLineEdit *title = nullptr, *incident = nullptr, *boot = nullptr, *logPath = nullptr;
    QLineEdit *frequency = nullptr, *changes = nullptr, *output = nullptr;
    QComboBox *category = nullptr, *logs = nullptr, *sockets = nullptr, *live = nullptr;
    QComboBox *excludedCollection = nullptr;
    QPlainTextEdit *happened = nullptr, *expected = nullptr, *steps = nullptr;
    QVBoxLayout *choices = nullptr;
    QListWidget *processes = nullptr, *files = nullptr;
    QSpinBox *duration = nullptr;
    QSpinBox *minutesBefore = nullptr, *minutesAfter = nullptr;
    QPlainTextEdit *preview = nullptr;
    QMap<QString, QCheckBox *> collections;

    void request(const QJsonObject &object)
    {
        busy = true;
        tabs->setEnabled(false);
        progress->show();
        stop->setVisible(object.value("action") == "collect");
        worker.write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
    }

    void fail(const QString &message)
    {
        busy = false;
        progress->hide();
        stop->hide();
        tabs->setEnabled(true);
        status->setText(message);
        if (!smokeOutput.isEmpty()) {
            qCritical("%s", qPrintable(message));
            QCoreApplication::exit(1);
        } else {
            QMessageBox::warning(this, "Report problem", message);
        }
    }

    void makeDescription()
    {
        auto *page = new QWidget;
        auto *form = new QFormLayout(page);
        title = new QLineEdit;
        category = new QComboBox;
        category->addItems({"Crash", "Freeze", "Rendering", "Performance", "Input", "Installation/update", "Other"});
        category->setCurrentText("Other");
        happened = new QPlainTextEdit;
        expected = new QPlainTextEdit;
        steps = new QPlainTextEdit;
        for (auto *field : {happened, expected, steps}) {
            field->setMaximumHeight(110);
            field->setPlaceholderText("Optional — leave empty if unknown.");
        }
        frequency = new QLineEdit("I don't know");
        changes = new QLineEdit("I don't know");
        incident = new QLineEdit(options.value("incident_time").toString());
        incident->setPlaceholderText("Optional ISO timestamp with timezone, e.g. 2026-10-08T13:20:00+02:00");
        boot = new QLineEdit(options.value("boot").toString("auto"));
        live = new QComboBox;
        live->addItems({"I don't know", "Yes", "No"});
        form->addRow("Short title", title);
        form->addRow("Problem category", category);
        form->addRow("What happened?", happened);
        form->addRow("What did you expect?", expected);
        form->addRow("Steps and applications involved", steps);
        form->addRow("How often does it happen?", frequency);
        form->addRow("Updates/settings changed afterward?", changes);
        form->addRow("Incident time", incident);
        form->addRow("Affected Vela session still running?", live);
        form->addRow("Journal boot: auto, 0=current, -1=previous, or ID", boot);
        minutesBefore = new QSpinBox;
        minutesAfter = new QSpinBox;
        minutesBefore->setRange(0, 120);
        minutesAfter->setRange(0, 120);
        minutesBefore->setValue(options.value("minutes_before").toInt(10));
        minutesAfter->setValue(options.value("minutes_after").toInt(5));
        form->addRow("Journal minutes before incident", minutesBefore);
        form->addRow("Journal minutes after incident", minutesAfter);
        form->addRow(paragraph("Current system/configuration data may differ from the incident. "
                              "Automatic boot selection follows the selected Vela log. "
                              "An unknown incident time uses its last write, or the whole selected boot."));
        auto *next = new QPushButton("Choose evidence");
        connect(next, &QPushButton::clicked, this, [this] { tabs->setCurrentIndex(1); });
        form->addRow(next);
        tabs->addTab(page, "1. Describe");
        connect(category, &QComboBox::currentTextChanged, this, [this] { recommend(); });
    }

    void makeEvidence()
    {
        auto *area = new QScrollArea;
        area->setWidgetResizable(true);
        auto *page = new QWidget;
        area->setWidget(page);
        choices = new QVBoxLayout(page);
        choices->addWidget(paragraph(privacy));
        auto *form = new QFormLayout;
        logs = new QComboBox;
        logs->addItem("No retained log / unknown session", QString());
        logPath = new QLineEdit(options.value("log").toString());
        auto *browse = new QPushButton("Choose log…");
        connect(browse, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, "Choose the Vela incident log", {}, {},
                                                          nullptr, QFileDialog::DontUseNativeDialog);
            if (!path.isEmpty())
                logPath->setText(path);
        });
        auto *row = new QHBoxLayout;
        row->addWidget(logPath);
        row->addWidget(browse);
        form->addRow("Retained Vela session", logs);
        form->addRow("Selected incident log", row);
        sockets = new QComboBox;
        sockets->addItem("No live Vela instance", QString());
        form->addRow("Live Vela command socket", sockets);
        duration = new QSpinBox;
        duration->setRange(1, 300);
        duration->setValue(options.value("duration").toInt(60));
        form->addRow("Recording duration (seconds)", duration);
        choices->addLayout(form);
        connect(logs, &QComboBox::currentIndexChanged, this, [this] {
            logPath->setText(logs->currentData().toString());
        });
        processes = new QListWidget;
        processes->setMaximumHeight(120);
        choices->addWidget(paragraph("For resource recording, select processes below. "
                                    "Each PID is tracked separately, including the supervisor."));
        choices->addWidget(processes);
        tabs->addTab(area, "2. Choose evidence");
    }

    void makeReview()
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->addWidget(paragraph(privacy + " Select a file to view its redacted contents. "
            "Binary attachments must be reviewed separately using the original file."));
        files = new QListWidget;
        files->setMaximumHeight(140);
        layout->addWidget(files);
        preview = new QPlainTextEdit;
        preview->setReadOnly(true);
        layout->addWidget(preview);
        connect(files, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
            if (item && !busy)
                request({{"action", "preview"}, {"name", item->data(Qt::UserRole).toString()}});
        });
        auto *actions = new QHBoxLayout;
        auto *remove = new QPushButton("Remove selected file");
        connect(remove, &QPushButton::clicked, this, [this] {
            if (files->currentItem()) {
                const auto name = files->currentItem()->data(Qt::UserRole).toString();
                if (name != "report.md" && name != "manifest.json")
                    request({{"action", "remove"}, {"name", name}});
            }
        });
        actions->addWidget(remove);
        auto *attach = new QPushButton("Add attachment…");
        connect(attach, &QPushButton::clicked, this, [this] { addAttachment(); });
        actions->addWidget(attach);
        auto *edit = new QPushButton("Update description");
        connect(edit, &QPushButton::clicked, this, [this] {
            request({{"action", "edit"}, {"description", description()}});
        });
        actions->addWidget(edit);
        layout->addLayout(actions);
        auto *exclusions = new QHBoxLayout;
        excludedCollection = new QComboBox;
        exclusions->addWidget(excludedCollection);
        auto *exclude = new QPushButton("Exclude collection");
        connect(exclude, &QPushButton::clicked, this, [this] {
            if (!excludedCollection->currentText().isEmpty())
                request({{"action", "exclude"}, {"collection", excludedCollection->currentText()}});
        });
        exclusions->addWidget(exclude);
        layout->addLayout(exclusions);
        auto *exportRow = new QHBoxLayout;
        output = new QLineEdit(options.value("output").toString());
        output->setPlaceholderText("ZIP output path");
        exportRow->addWidget(output);
        auto *browse = new QPushButton("Save as…");
        connect(browse, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getSaveFileName(this, "Report ZIP destination", output->text(),
                                                           "ZIP archives (*.zip)", nullptr,
                                                           QFileDialog::DontUseNativeDialog);
            if (!path.isEmpty())
                output->setText(path);
        });
        exportRow->addWidget(browse);
        auto *save = new QPushButton("Export reviewed ZIP");
        connect(save, &QPushButton::clicked, this, [this] { exportReport(); });
        exportRow->addWidget(save);
        layout->addLayout(exportRow);
        tabs->addTab(page, "3. Review and export");
        tabs->setTabEnabled(2, false);
    }

    QJsonObject description() const
    {
        return {{"title", title->text()}, {"category", category->currentText()},
                {"what_happened", happened->toPlainText()}, {"expected", expected->toPlainText()},
                {"steps", steps->toPlainText()}, {"frequency", frequency->text()},
                {"changed_since_incident", changes->text()}};
    }

    void recommend()
    {
        QStringList selected;
        for (const auto &value : discovered.value("recommendations").toObject()
                                     .value(category->currentText()).toArray())
            selected.append(value.toString());
        for (auto it = collections.begin(); it != collections.end(); ++it)
            it.value()->setChecked(selected.contains(it.key()));
    }

    void collect()
    {
        if (!smokeOutput.isEmpty()) {
            request({{"action", "collect"}, {"selected", QJsonArray{}},
                     {"context", QJsonObject{}}, {"description", QJsonObject{{"title", "GUI smoke test"}}}});
            return;
        }
        if (!incident->text().isEmpty()) {
            static const QRegularExpression offset("(Z|[+-][0-9]{2}:[0-9]{2})$");
            if (!offset.match(incident->text()).hasMatch() ||
                !QDateTime::fromString(incident->text(), Qt::ISODate).isValid()) {
                QMessageBox::warning(this, "Incident time", "Use an ISO timestamp including its timezone offset.");
                return;
            }
        }
        QJsonArray selected, identities;
        for (auto it = collections.begin(); it != collections.end(); ++it) {
            if (it.value()->isChecked())
                selected.append(it.key());
        }
        for (int i = 0; i < processes->count(); ++i) {
            auto *item = processes->item(i);
            if (item->checkState() == Qt::Checked)
                identities.append(item->data(Qt::UserRole).toJsonObject());
        }
        if (QMessageBox::question(this, "Accept collection plan",
            "Collect the selected evidence?\n\n" + privacy +
            "\n\nIf recording resources, repeat the steps that trigger the problem during the countdown.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        QJsonObject context{{"log_path", logPath->text()}, {"socket", sockets->currentData().toString()},
                            {"incident_time", incident->text()}, {"boot", boot->text()},
                            {"processes", identities}, {"duration", duration->value()},
                            {"minutes_before", minutesBefore->value()}, {"minutes_after", minutesAfter->value()},
                            {"live_during_incident", live->currentText()}};
        request({{"action", "collect"}, {"selected", selected}, {"context", context},
                 {"description", description()}, {"description_file", options.value("description_file")}});
    }

    void addAttachment()
    {
        const auto path = QFileDialog::getOpenFileName(this, "Choose a specific attachment to include", {}, {},
                                                       nullptr, QFileDialog::DontUseNativeDialog);
        if (path.isEmpty())
            return;
        QDialog dialog(this);
        dialog.setWindowTitle("Include attachment");
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(paragraph("Include this file only after reviewing its contents.\n" + privacy +
            "\nCore dumps contain process memory and can expose credentials or application data."));
        auto *kind = new QComboBox;
        kind->addItems({"text", "update", "screenshot", "video", "core", "vendor", "other"});
        kind->setCurrentText("other");
        layout->addWidget(paragraph("Choose text or update only for a plain-text file; other types stay unchanged."));
        layout->addWidget(kind);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Include this file");
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() == QDialog::Accepted)
            request({{"action", "attach"}, {"path", path}, {"kind", kind->currentText()}});
    }

    void exportReport()
    {
        const auto path = output->text();
        if (path.isEmpty())
            return;
        bool overwrite = false;
        if (QFileInfo::exists(path)) {
            overwrite = QMessageBox::question(this, "Replace existing report",
                "This file exists. Replace it?", QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) == QMessageBox::Yes;
            if (!overwrite)
                return;
        }
        if (QMessageBox::question(this, "Export reviewed report",
                "Create the ZIP with the reviewed contents?", QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) == QMessageBox::Yes)
            request({{"action", "export"}, {"path", path}, {"overwrite", overwrite}});
    }

    void updateSummary(const QJsonObject &result)
    {
        summary = result;
        files->blockSignals(true);
        files->clear();
        const auto artifacts = result.value("artifacts").toObject();
        for (const auto &name : QStringList{"report.md", "manifest.json"} + artifacts.keys()) {
            const auto item = artifacts.value(name).toObject();
            auto *row = new QListWidgetItem(name + (item.isEmpty() ? "" :
                QStringLiteral(" — %1 bytes%2").arg(item.value("size").toInteger()).arg(
                    item.value("sensitive").toBool() ? " — sensitive" : "")), files);
            row->setData(Qt::UserRole, name);
        }
        files->blockSignals(false);
        excludedCollection->clear();
        QStringList statuses;
        const auto results = result.value("results").toObject();
        for (auto it = results.begin(); it != results.end(); ++it) {
            excludedCollection->addItem(it.key());
            const auto value = it.value().toObject();
            statuses << it.key() + ": " + value.value("status").toString() +
                        " — " + value.value("reason").toString();
        }
        preview->setPlainText(statuses.join('\n'));
        if (output->text().isEmpty())
            output->setText(result.value("default_output").toString());
        tabs->setTabEnabled(2, true);
        tabs->setCurrentIndex(2);
        exported = false;
    }

    void receive(const QJsonObject &message)
    {
        const auto event = message.value("event").toString();
        if (event == "progress") {
            status->setText(message.value("message").toString());
            return;
        }
        busy = false;
        tabs->setEnabled(true);
        progress->hide();
        stop->hide();
        if (event == "error") {
            fail(message.value("message").toString());
            return;
        }
        const auto action = message.value("action").toString();
        const auto result = message.value("result").toObject();
        if (action == "discover") {
            discovered = result;
            for (const auto &value : result.value("logs").toArray()) {
                const auto log = value.toObject();
                logs->addItem(log.value("label").toString() + " — " + log.value("modified").toString(),
                              log.value("path").toVariant());
            }
            if (logs->count() > 1 && logPath->text().isEmpty())
                logs->setCurrentIndex(1);
            for (const auto &value : result.value("sockets").toArray())
                sockets->addItem(value.toString(), value.toString());
            for (const auto &value : result.value("processes").toArray()) {
                const auto identity = value.toObject();
                auto *item = new QListWidgetItem(identity.value("role").toString() +
                    QStringLiteral(" — PID %1").arg(identity.value("pid").toInt()), processes);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Unchecked);
                item->setData(Qt::UserRole, QVariant::fromValue(identity));
            }
            for (const auto &value : result.value("collections").toArray()) {
                const auto spec = value.toObject();
                const auto id = spec.value("id").toString();
                auto *check = new QCheckBox(spec.value("title").toString());
                check->setToolTip(spec.value("help").toString() + "\nPrivacy: " + spec.value("privacy").toString());
                choices->addWidget(check);
                choices->addWidget(paragraph(spec.value("help").toString() +
                    "\nPrivacy: " + spec.value("privacy").toString()));
                collections[id] = check;
                if (id == "vendor") {
                    check->setEnabled(result.value("vendor_available").toBool());
                    connect(check, &QCheckBox::toggled, this, [this, check](bool checked) {
                        if (checked && QMessageBox::question(this, "NVIDIA diagnostic report",
                            "Generate an unredacted NVIDIA report as your user? "
                            "It may contain additional system details. Review it separately before sharing. "
                            "You can instead attach an existing report.",
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                            check->setChecked(false);
                    });
                }
            }
            auto *start = new QPushButton("Collect selected evidence");
            connect(start, &QPushButton::clicked, this, [this] { collect(); });
            choices->addWidget(start);
            recommend();
            status->setText("Describe the problem, select the incident and choose evidence.");
            if (!smokeOutput.isEmpty())
                collect();
        } else if (action == "collect" || action == "remove" || action == "attach" ||
                   action == "exclude" || action == "edit") {
            updateSummary(result);
            status->setText("Review the collector results and included files before export.");
            if (!smokeOutput.isEmpty())
                request({{"action", "preview"}, {"name", "report.md"}});
        } else if (action == "preview") {
            preview->setPlainText(result.value("text").toString());
            if (!smokeOutput.isEmpty())
                request({{"action", "export"}, {"path", smokeOutput}});
        } else if (action == "export") {
            exported = true;
            status->setText(QStringLiteral("Report created: %1 (%2 bytes). Nothing was uploaded.")
                            .arg(result.value("path").toString()).arg(result.value("size").toInteger()));
            if (!smokeOutput.isEmpty())
                QCoreApplication::exit(0);
            else
                QMessageBox::information(this, "Report created", status->text());
        }
    }
};

int main(int argc, char **argv)
{
    vela::install_new_handler("vela-report-gui");
    QApplication application(argc, argv);
    application.setApplicationName("Vela Report");
    application.setDesktopFileName("vela-report");
    const auto arguments = application.arguments();
    if (arguments.contains("--probe"))
        return 0;
    std::signal(SIGTERM, [](int) { interrupted = 1; });
    std::signal(SIGINT, [](int) { interrupted = 1; });
    QTimer interruptTimer;
    QObject::connect(&interruptTimer, &QTimer::timeout, &application, [&application] {
        if (interrupted)
            application.exit(130);
    });
    interruptTimer.start(100);
    auto option = [&arguments](const QString &name, const QString &fallback = {}) {
        const auto index = arguments.indexOf(name);
        return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : fallback;
    };
    const auto options = QJsonDocument::fromJson(option("--options-json", "{}").toUtf8()).object();
    const auto smokeOutput = option("--smoke-test");
    Reporter reporter(option("--python", QStandardPaths::findExecutable("python3")), options, smokeOutput);
    reporter.show();
    if (!smokeOutput.isEmpty())
        QTimer::singleShot(20000, &application, [&application] { application.exit(1); });
    return application.exec();
}
