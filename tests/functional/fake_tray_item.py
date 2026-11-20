#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""A notification area icon for the functional tests: a StatusNotifierItem
with an Id and a solid-color picture, registered with the shell's watcher.

    fake_tray_item.py <id> <rrggbb>

It prints "ready" once registered, and runs until killed."""

import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

ITEM = "org.kde.StatusNotifierItem"
PROPERTIES = "org.freedesktop.DBus.Properties"
WATCHER = "org.kde.StatusNotifierWatcher"


class Item(dbus.service.Object):
    def __init__(self, bus, item_id, color):
        super().__init__(bus, "/StatusNotifierItem")
        size = 22
        red, green, blue = (int(color[i:i + 2], 16) for i in (0, 2, 4))
        pixel = bytes([255, red, green, blue]) # ARGB, network order
        picture = dbus.Struct((dbus.Int32(size), dbus.Int32(size), dbus.ByteArray(pixel * size * size)),
                              signature="iiay")
        self.properties = {
            "Category": "ApplicationStatus",
            "Id": item_id,
            "Title": f"Fake {item_id}",
            "Status": "Active",
            "IconName": "",
            "IconPixmap": dbus.Array([picture], signature="(iiay)"),
            "ItemIsMenu": False,
            "Menu": dbus.ObjectPath("/"),
        }

    @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.properties if interface == ITEM else {}

    @dbus.service.method(PROPERTIES, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.properties[name]

    @dbus.service.method(ITEM, in_signature="ii")
    def Activate(self, x, y):
        pass

    @dbus.service.method(ITEM, in_signature="ii")
    def SecondaryActivate(self, x, y):
        pass

    @dbus.service.method(ITEM, in_signature="is")
    def Scroll(self, delta, orientation):
        pass


def main():
    item_id, color = sys.argv[1], sys.argv[2]
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    item = Item(bus, item_id, color)
    watcher = bus.get_object(WATCHER, "/StatusNotifierWatcher")
    watcher.RegisterStatusNotifierItem(bus.get_unique_name(), dbus_interface=WATCHER)
    print("ready", flush=True)
    GLib.MainLoop().run()
    del item


if __name__ == "__main__":
    main()
