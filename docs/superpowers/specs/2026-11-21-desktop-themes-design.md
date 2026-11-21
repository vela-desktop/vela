# Desktop themes in Settings

## Intent

Let users choose system fonts, cursor style, and icon theme in Vela Settings so Qt/KDE applications share the same appearance when Plasma is installed.

## Design

Add a Themes page under Personalization, matching existing Settings cards. Discover available font families, cursor themes, and icon themes from the system; selecting one writes the corresponding KDE configuration and sends the KDE/Qt notification needed for other applications to reload. Vela's compositor reloads its cursor and title font in the current session. KDE's configuration remains the source of truth, including when Plasma is not running.

## Scope and constraints

Only expose choices that are installed and discoverable. Preserve KDE's unrelated configuration values. Do not add preview, font sizing, GTK-specific integration, or theme installation.

## Approaches considered

1. KDE configuration as source of truth (selected): lets Plasma and Qt apps follow Vela's choices and matches the existing color-scheme integration.
2. Vela-owned settings applied only to Vela apps: simpler isolation, but does not meet cross-desktop consistency.

Implementation details remain with the builder, following the existing Settings controls and KDE integration patterns.
