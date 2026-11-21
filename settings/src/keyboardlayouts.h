// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>

// The keyboard layouts XKB knows, for Keyboard > Layouts (docs/keyboard.md
// §5): from libxkbregistry, with the extras and the user's own rules, named
// in Vela's language with xkeyboard-config's translations, grouped by layout
// and searchable. Read the first time they're needed.
class KeyboardLayouts : public QObject {
    Q_OBJECT
    // Bumped when the names change language: pass it to name() and groups()
    // so the bindings follow.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // How many layouts Vela can switch between (docs/keyboard.md §5.1).
    Q_PROPERTY(int maxLayouts READ maxLayouts CONSTANT)

public:
    static constexpr int MaxLayouts = 4;

    explicit KeyboardLayouts(QObject* parent = nullptr);
    // For the tests: the registry from `includePaths` only, translations
    // from `localeDir`/<language>/LC_MESSAGES.
    KeyboardLayouts(const QStringList& includePaths, const QString& localeDir, QObject* parent = nullptr);

    int revision() const { return m_revision; }
    int maxLayouts() const { return MaxLayouts; }

    // "it" or "en": the language of the names (shell/src/language.h).
    void setLanguage(const QString& language);

    // The groups matching `query` (every word, ignoring case and accents, in
    // the translated or English name, the code, or the languages' and
    // countries' names), sorted by name: [{layout, name, matches, variants:
    // [{variant, name}]}]. A group whose own entry matches has all its
    // variants and `matches` true; otherwise only the matching variants.
    Q_INVOKABLE QVariantList groups(const QString& query, int revision = 0);
    // The name to show ("Italiana", "English (US, intl.)...").
    Q_INVOKABLE QString name(const QString& layout, const QString& variant, int revision = 0);

    // Where the layout in use comes from when Settings has none, in the
    // compositor's order (compositor/src/keyboard.c, docs/keyboard.md §5.5):
    // {source: "environment" | "system" | "default", layouts: [{layout,
    // variant}]}. "environment": XKB_DEFAULT_LAYOUT is set, and wins even
    // over the layouts chosen in Settings.
    Q_INVOKABLE QVariantMap source() const;
    // The same from given values, for the tests: XKB_DEFAULT_LAYOUT and
    // XKB_DEFAULT_VARIANT, and the text of systemd-localed's
    // 00-keyboard.conf.
    static QVariantMap sourceFrom(const QString& envLayout, const QString& envVariant, const QString& localed);

    // A gettext catalog (.mo): original → translation. Empty if it isn't
    // one.
    static QHash<QString, QString> readCatalog(const QByteArray& data);
    // Lowercase, without accents: what the search compares.
    static QString fold(const QString& text);

signals:
    void changed();

private:
    struct Entry {
        QString layout;
        QString variant;
        QString english;
        QString name;
        QStringList countries; // ISO 3166 alpha-2
        QStringList languages; // ISO 639-3
        QString search; // folded: names, codes, languages, countries
    };
    struct Group {
        int layout = -1; // the entry without a variant, in m_entries
        QList<int> variants;
    };

    void load();
    void translate();
    const Entry* find(const QString& layout, const QString& variant);

    QStringList m_includePaths; // empty: libxkbcommon's defaults
    QString m_localeDir;
    QString m_language = QStringLiteral("en");
    bool m_loaded = false;
    int m_revision = 0;
    QList<Entry> m_entries;
    QList<Group> m_groups; // sorted by the layout's name
};
