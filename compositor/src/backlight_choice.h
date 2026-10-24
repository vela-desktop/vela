// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BACKLIGHT_CHOICE_H
#define VELA_BACKLIGHT_CHOICE_H

// Which /sys/class/backlight device drives a connector (docs/brightness.md
// §5.1). Pure apart from reading sysfs, so the tests use a tree of their own.
//
// - A backlight whose parent is the connector itself is its own, whatever
//   the connector.
// - A built-in connector (eDP, LVDS, DSI) otherwise takes one whose parent is
//   its DRM card (or the card's device), then any, each time by type:
//   firmware, platform, raw; by name among equals.
// - Anything else drives no backlight: it is DDC/CI's.

#include <stdbool.h>
#include <stddef.h>

// `connector`: "card1-eDP-1" (or a name without a card, for the tests).
bool vela_backlight_choose(const char *sysfs, const char *connector, char *device, size_t size);

// eDP, LVDS or DSI, with or without the card: a screen inside the computer.
bool vela_connector_is_internal(const char *connector);

#endif
