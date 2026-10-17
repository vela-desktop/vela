// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// File Explorer's views under Qt Quick Test (tst_*.qml next to this one), on
// a real FolderModel over a folder of files made here: a recycled row or cell
// takes its new file's state at once (docs/visual-coherence.md §6.8). A
// private vela.conf sets the durations three times Vela's, so a fade still
// running would be caught.

#include "controls.h"
#include "foldermodel.h"
#include "iconprovider.h"

#include <QDir>
#include <QVariantList>
#include <QStandardItemModel>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QtQml/qqmlextensionplugin.h>
#include <QtQuickTest/quicktest.h>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

namespace {
QString folder;
}

class TestTransferOps : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList jobs READ jobs WRITE setJobs NOTIFY jobsChanged)
    Q_PROPERTY(QAbstractItemModel* jobModel READ jobModel CONSTANT)
    Q_PROPERTY(int lastCancelled READ lastCancelled NOTIFY cancelledChanged)
public:
    TestTransferOps()
    {
        m_model.setItemRoleNames({ { Qt::UserRole + 1, QByteArrayLiteral("modelData") } });
    }
    QVariantList jobs() const { return m_jobs; }
    QAbstractItemModel* jobModel() { return &m_model; }
    void setJobs(const QVariantList& jobs)
    {
        m_jobs = jobs;
        m_model.clear();
        m_model.setItemRoleNames({ { Qt::UserRole + 1, QByteArrayLiteral("modelData") } });
        for (const QVariant& job : jobs) {
            auto* item = new QStandardItem;
            item->setData(job.toMap(), Qt::UserRole + 1);
            m_model.appendRow(item);
        }
        emit jobsChanged();
    }
    Q_INVOKABLE void updateJob(int id, int done, int total)
    {
        for (int row = 0; row < m_model.rowCount(); ++row) {
            QStandardItem* item = m_model.item(row);
            QVariantMap data = item->data(Qt::UserRole + 1).toMap();
            if (data.value(QStringLiteral("id")).toInt() != id) {
                continue;
            }
            data[QStringLiteral("done")] = done;
            data[QStringLiteral("total")] = total;
            item->setData(data, Qt::UserRole + 1);
            return;
        }
    }
    int lastCancelled() const { return m_lastCancelled; }
    Q_INVOKABLE void cancelJob(int id) { m_lastCancelled = id; emit cancelledChanged(); }
    Q_INVOKABLE void dismissJob(int id)
    {
        for (qsizetype i = 0; i < m_jobs.size(); ++i) {
            if (m_jobs[i].toMap().value(QStringLiteral("id")).toInt() == id) {
                m_jobs.removeAt(i);
                m_model.removeRow(i);
                emit jobsChanged();
                break;
            }
        }
    }
signals:
    void jobsChanged();
    void cancelledChanged();
private:
    QVariantList m_jobs;
    QStandardItemModel m_model;
    int m_lastCancelled = -1;
};

class Setup : public QObject {
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        vela::controls::install(*engine, Appearance::Mode::Apps);
        engine->addImageProvider(QStringLiteral("fileicon"), new IconProvider(32));
        engine->rootContext()->setContextProperty(QStringLiteral("TestFolder"), folder);
        engine->rootContext()->setContextProperty(QStringLiteral("Ops"), &m_ops);
    }
private:
    TestTransferOps m_ops;
};

int main(int argc, char** argv)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
        qputenv("QT_QUICK_BACKEND", "software");
    }
    QDir().mkpath(home.filePath(QStringLiteral("config/vela")));
    QFile conf(home.filePath(QStringLiteral("config/vela/vela.conf")));
    if (!conf.open(QIODevice::WriteOnly)) {
        return 1;
    }
    conf.write("animation-duration=300\n");
    conf.close();

    // 300 files, sorted by name: file-000 … file-299.
    folder = home.filePath(QStringLiteral("files"));
    QDir().mkpath(folder);
    for (int i = 0; i < 300; ++i) {
        QFile file(QStringLiteral("%1/file-%2.txt").arg(folder).arg(i, 3, 10, QLatin1Char('0')));
        if (!file.open(QIODevice::WriteOnly)) {
            return 1;
        }
    }

    qmlRegisterType<FolderModel>("Vela.Files.Backend", 1, 0, "FolderModel");
    Setup setup;
    return quick_test_main_with_setup(argc, argv, "files-views", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "views_test.moc"
