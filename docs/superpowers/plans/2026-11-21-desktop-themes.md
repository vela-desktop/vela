# Desktop themes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add font, cursor, and icon theme choices to Vela Settings and apply them through KDE's existing configuration.

**Architecture:** Add one Themes page under Personalization. Extend `Preferences` to read installed choices and persist selection to KDE configuration, following the existing `setAppTheme` integration pattern. Keep KDE's configuration as source of truth.

**Tech Stack:** Qt 6, Qt Quick, QSettings, KDE configuration tools and session D-Bus.

**Spec:** `docs/superpowers/specs/2026-11-21-desktop-themes-design.md`

## Global Constraints

- Only expose installed and discoverable choices.
- Preserve unrelated KDE configuration values.
- Do not add preview, font sizing, GTK-specific integration, or theme installation.

## Review Focus

- KDE utilities may be absent: the page must remain usable and avoid failed writes when KDE configuration tools are unavailable.
- A system may have no non-default themes: selectors must still show the current/default value.
- Theme names may contain spaces or punctuation: pass them as individual process arguments and preserve them verbatim.
- Font family lists can be large: use a searchable or otherwise usable existing choice control rather than rendering a huge list inline.
- KDE may cache appearance: trigger the appropriate session notification after applying each selection.

---

### Task 1: Add KDE desktop appearance choices

**Files:**
- Create: `settings/qml/pages/ThemesPage.qml`
- Modify: `settings/qml/Main.qml` (page registry and setting search index)
- Modify: `settings/qml/pages/PersonalizationPage.qml` (page entry)
- Modify: `settings/src/preferences.h` and `settings/src/preferences.cpp` (available choices, current values, persistence)
- Modify: `settings/CMakeLists.txt` (register the page)
- Modify: `settings/i18n/vela-settings_it.ts` (Italian strings for the new controls)
- Modify: `shell/src/appearance.cpp`, `shell/src/appearance.h`, `shell/src/iconprovider.h`, and `common/controls/Theme.qml` (reload cached shell icons on KDE theme changes)
- Modify: `shell/qml/NotificationCenter.qml`, `shell/qml/NotificationPopups.qml`, `shell/qml/Taskbar.qml`, `shell/qml/Wallpaper.qml`, `shell/qml/PropertiesDialog.qml`, and `shell/qml/FileDialogs.qml` (refresh current icon URLs)
- Modify: `compositor/src/input.c`, `compositor/src/input.h`, `compositor/src/command.c`, `compositor/src/view.c`, `compositor/src/view.h`, `compositor/src/decoration.c`, and `compositor/src/decoration.h` (reload current-session cursor and title font)

**Interfaces:**
- `Preferences` exposes current font, cursor theme, and icon theme values, lists of installed choices, and write properties for each.
- The QML page uses existing `Card`, `CardGroup`, and `Choice` controls.

- [ ] Read the KDE configuration keys and refresh notifications used by the installed Qt/KDE integration; keep font, cursor, and icon values in their existing KDE config groups. Font and cursor changes also reload Vela's current-session compositor appearance.
- [ ] Add `Preferences` properties that read the current selections, enumerate installed font/cursor/icon choices, and write validated selections without changing unrelated keys.
- [ ] Add the Themes page with one selection card for each appearance choice, using the closest matching existing Settings controls.
- [ ] Register the page under Personalization and make its settings searchable by font, cursor, pointer, and icon terms.
- [ ] Build `vela-settings` and inspect the final diff for scope and config preservation.
