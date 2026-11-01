// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Updates as Settings and the shell show them (docs/updates.md §8): a
// client of vela-update-daemon's org.vela.Updates, its properties as QML
// reads them. Without the daemon `available` is false and everything else
// is as before any check; when it comes back it is read again. The
// automatic checks switch shows what the user asked for until the daemon
// reports it (requested.h). The error codes come with their reason in
// plain words.

#include "requested.h"

#include <QDBusConnection>
#include <QDateTime>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QDBusMessage;

namespace vela::updates {

class Updates : public QObject {
    Q_OBJECT
    // The daemon runs.
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // "idle", "checking", "starting" (the terminal is opening),
    // "installing"; busy: not idle.
    Q_PROPERTY(QString activity READ activity NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // While installing: "resolving", "downloading", "verifying", "building",
    // "installing" (§5.2).
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    // The last check (§5.1): status is "up-to-date", "update-available",
    // "downgrade-available", "not-installed", "error", or "unknown" before
    // any check.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString channel READ channel NOTIFY changed)
    Q_PROPERTY(QString installedPackage READ installedPackage NOTIFY changed)
    Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY changed)
    // The update found.
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(QString tag READ tag NOTIFY changed)
    Q_PROPERTY(QString commit READ commit NOTIFY changed)
    // The version as the user reads it: Experimental's has the commit too,
    // since many commits share one version ("0.1.1 (1a2b3c4)").
    Q_PROPERTY(QString versionText READ versionText NOTIFY changed)
    // Experimental's commits, newest first: [{commit, summary}]; at most
    // 100 of changesTotal.
    Q_PROPERTY(QVariantList changes READ changes NOTIFY changed)
    Q_PROPERTY(int changesTotal READ changesTotal NOTIFY changed)
    // Beta's release texts, newest first: [{version, text}].
    Q_PROPERTY(QVariantList notes READ notes NOTIFY changed)
    // Experimental's source.
    Q_PROPERTY(QString source READ source NOTIFY changed)
    // Invalid if never checked.
    Q_PROPERTY(QDateTime lastCheck READ lastCheck NOTIFY changed)
    // The last check's failure, beside the last completed check: {code,
    // message, reason}; empty if it didn't fail. reason: the code in plain
    // words; message: vela-update's own, in English.
    Q_PROPERTY(QVariantMap checkError READ checkError NOTIFY changed)
    // The last install: "success", "failed", "cancelled", or empty.
    Q_PROPERTY(QString installResult READ installResult NOTIFY changed)
    // Its failure, as checkError.
    Q_PROPERTY(QVariantMap installError READ installError NOTIFY changed)
    // When it ended; invalid if never.
    Q_PROPERTY(QDateTime installFinished READ installFinished NOTIFY changed)
    // An update replaced the running compositor: "Sign out to finish".
    Q_PROPERTY(bool needsRelogin READ needsRelogin NOTIFY changed)
    // Automatic checks, or what was asked for them.
    Q_PROPERTY(bool autoCheck READ autoCheck NOTIFY changed)
    // The commit of an update to tell the user about, once (§6.4); empty
    // when there's none.
    Q_PROPERTY(QString announce READ announce NOTIFY changed)
    // The last refusal of something asked here, in plain words; empty if
    // none.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit Updates(QObject* parent = nullptr);
    Updates(const QDBusConnection& bus, QObject* parent = nullptr);

    bool available() const { return m_running; }
    QString activity() const { return m_activity; }
    bool busy() const { return m_activity != QLatin1String("idle"); }
    QString phase() const { return m_phase; }
    QString status() const { return m_status; }
    QString channel() const { return m_channel; }
    QString installedPackage() const { return m_installedPackage; }
    QString installedVersion() const { return m_installedVersion; }
    QString version() const { return m_version; }
    QString tag() const { return m_tag; }
    QString commit() const { return m_commit; }
    QString versionText() const;
    QVariantList changes() const { return m_changes; }
    int changesTotal() const { return m_changesTotal; }
    QVariantList notes() const { return m_notes; }
    QString source() const { return m_source; }
    QDateTime lastCheck() const;
    QVariantMap checkError() const { return failure(m_checkError, m_checkMessage); }
    QString installResult() const { return m_installResult; }
    QVariantMap installError() const { return failure(m_installError, m_installMessage); }
    QDateTime installFinished() const;
    bool needsRelogin() const { return m_needsRelogin; }
    bool autoCheck() const { return m_autoCheckAsked.value(m_autoCheck); }
    QString announce() const { return m_announce; }
    QString error() const { return m_error; }

    // An error code (§5.4) in plain words.
    Q_INVOKABLE QString reason(const QString& code) const;

    Q_INVOKABLE void check();
    // Opens the update in a terminal (§6.3). options: channel, branch,
    // commit, source, tag; all optional, empty ones left out.
    Q_INVOKABLE void install(const QVariantMap& options = {});
    Q_INVOKABLE void setAutoCheck(bool on);
    // The update `commit` was told to the user (§6.4).
    Q_INVOKABLE void announced(const QString& commit);
    Q_INVOKABLE void clearError();

signals:
    void changed();
    void errorChanged();

private Q_SLOTS:
    void onOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);
    void onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    // A refusal is `shown` in error(), or only logged when the user didn't
    // ask for the call.
    void call(const QString& name, const QVariantList& arguments, bool shown,
        const std::function<void(const QDBusMessage&)>& done = {});
    void appeared();
    void vanished();
    // What is known without the daemon.
    void forget();
    void setProperties(const QVariantMap& values);
    QVariantMap failure(const QString& code, const QString& message) const;
    void refused(const QDBusMessage& reply);
    void setError(const QString& error);

    QDBusConnection m_bus;
    quint64 m_ownerGeneration = 0; // ignore replies from a previous daemon
    bool m_running = false;
    QString m_activity;
    QString m_phase;
    QString m_status;
    QString m_channel;
    QString m_installedPackage;
    QString m_installedVersion;
    QString m_version;
    QString m_tag;
    QString m_commit;
    QVariantList m_changes;
    int m_changesTotal = 0;
    QVariantList m_notes;
    QString m_source;
    qint64 m_lastCheck = 0;
    QString m_checkError;
    QString m_checkMessage;
    QString m_installResult;
    QString m_installError;
    QString m_installMessage;
    qint64 m_installFinished = 0;
    bool m_needsRelogin = false;
    bool m_autoCheck = true;
    QString m_announce;
    QString m_error;
    Requested m_autoCheckAsked;
};

} // namespace vela::updates
