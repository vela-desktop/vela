// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Vela's interface language: Italian or English.
//
// It's chosen in Settings > Time & language > Language, key language= of
// ~/.config/vela/vela.conf: "it", "en" or nothing (like the system: Italian if
// the system is in Italian, English otherwise). Strings in the code are in
// English; Italian is a Qt translation (i18n/*_it.ts, compiled into the
// executable). The default locale changes with the language: dates, months and
// days follow the choice.
//
// Every app watches vela.conf: when the language changes, the translation is
// reloaded and `changed` retranslates the interface (QQmlEngine::retranslate),
// without restarting anything.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLocale>
#include <QStandardPaths>
#include <QString>
#include <QTranslator>

#include <functional>

namespace vela::language {

inline QString configPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/vela/vela.conf");
}

// The choice in vela.conf: "it", "en", or empty (like the system).
inline QString configured()
{
    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QString value;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        // "lingua=" is the old name (compositor/src/legacy_names.c).
        if (line.startsWith(QLatin1String("language="))) {
            value = line.mid(9).trimmed();
        } else if (line.startsWith(QLatin1String("lingua="))) {
            value = line.mid(7).trimmed();
        }
    }
    return value == QLatin1String("it") || value == QLatin1String("en") || value == QLatin1String("pt")
        ? value
        : QString();
}

// The language to use: "it", "pt" or "en".
inline QString effective()
{
    const QString chosen = configured();
    if (!chosen.isEmpty()) {
        return chosen;
    }
    const auto sysLang = QLocale::system().language();
    if (sysLang == QLocale::Italian) {
        return QStringLiteral("it");
    }
    if (sysLang == QLocale::Portuguese) {
        return QStringLiteral("pt");
    }
    return QStringLiteral("en");
}

// Loads translation `name` (the :/i18n/<name>_it.qm resource) and keeps it in
// line with vela.conf. Call once, after creating the app.
inline void install(const QString& name, std::function<void()> changed = {})
{
    struct State {
        QString name;
        std::function<void()> changed;
        QTranslator* translator = nullptr;
        QString current;
    };
    auto* state = new State { name, std::move(changed), nullptr, {} };
    auto apply = [state] {
        const QString lang = effective();
        if (lang == state->current) {
            return;
        }
        const bool first = state->current.isEmpty();
        state->current = lang;
        if (state->translator) {
            QCoreApplication::removeTranslator(state->translator);
            delete state->translator;
            state->translator = nullptr;
        }
        const QLocale::Territory territory = QLocale::system().territory();
        const QLocale::Language qtLang = lang == QLatin1String("en") ? QLocale::English
            : lang == QLatin1String("pt")                            ? QLocale::Portuguese
                                                                     : QLocale::Italian;
        QLocale::setDefault(QLocale(qtLang, territory));
        if (lang != QLatin1String("en")) {
            state->translator = new QTranslator(QCoreApplication::instance());
            if (state->translator->load(QStringLiteral(":/i18n/%1_%2.qm").arg(state->name, lang))) {
                QCoreApplication::installTranslator(state->translator);
            }
        }
        if (!first && state->changed) {
            state->changed();
        }
    };
    apply();

    // vela.conf is replaced with a rename: the directory is watched too, and
    // the file is added again every time it reappears.
    auto* watcher = new QFileSystemWatcher(QCoreApplication::instance());
    const QString path = configPath();
    const QString directory = QFileInfo(path).absolutePath();
    QDir().mkpath(directory);
    watcher->addPath(directory);
    if (QFile::exists(path)) {
        watcher->addPath(path);
    }
    auto refresh = [watcher, path, apply] {
        if (QFile::exists(path) && !watcher->files().contains(path)) {
            watcher->addPath(path);
        }
        apply();
    };
    QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, watcher, refresh);
    QObject::connect(watcher, &QFileSystemWatcher::fileChanged, watcher, refresh);
}

} // namespace vela::language
