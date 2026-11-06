#!/usr/bin/env python3
"""Stand-in org.freedesktop.Notifications server for tests.

Logs every Notify call as a JSON line (with the image size instead of its bytes).
--press-next N presses the "next" action on the N-th notification, one second after it.
"""
import argparse
import json
import sys
import time

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib


START = time.monotonic()


class Server(dbus.service.Object):
    def __init__(self, bus, log, press_next):
        super().__init__(bus, "/org/freedesktop/Notifications")
        self.log = open(log, "a", buffering=1)
        self.press_next = press_next
        self.count = 0
        self.next_id = 1

    @dbus.service.method("org.freedesktop.Notifications", in_signature="", out_signature="as")
    def GetCapabilities(self):
        return ["actions", "body", "body-markup", "icon-static", "persistence"]

    @dbus.service.method("org.freedesktop.Notifications", in_signature="", out_signature="ssss")
    def GetServerInformation(self):
        return ("fake", "test", "1", "1.2")

    @dbus.service.method("org.freedesktop.Notifications", in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app, replaces, icon, summary, body, actions, hints, timeout):
        nid = int(replaces) if replaces else self.next_id
        if not replaces:
            self.next_id += 1
        self.count += 1
        img = hints.get("image-data")
        entry = {
            "t": round(time.monotonic() - START, 2), "n": self.count, "id": nid, "replaces": int(replaces), "app": str(app), "icon": str(icon),
            "summary": str(summary), "body": str(body), "actions": [str(a) for a in actions],
            "hints": sorted(str(k) for k in hints.keys()),
            "image": [int(img[0]), int(img[1]), int(img[5]), len(img[6])] if img is not None else None,
            # centre pixel, to tell cover art from the fallback logo
            "pixel": list(bytes(img[6][(int(img[1]) // 2) * int(img[2]) + (int(img[0]) // 2) * int(img[5]):][:int(img[5])])) if img is not None else None,
            "timeout": int(timeout),
        }
        self.log.write(json.dumps(entry, ensure_ascii=False) + "\n")
        if self.count == self.press_next and "next" in entry["actions"]:
            GLib.timeout_add(1000, lambda: (self.ActionInvoked(dbus.UInt32(nid), "next"), False)[1])
        return dbus.UInt32(nid)

    @dbus.service.method("org.freedesktop.Notifications", in_signature="u", out_signature="")
    def CloseNotification(self, nid):
        self.NotificationClosed(nid, dbus.UInt32(3))

    @dbus.service.signal("org.freedesktop.Notifications", signature="us")
    def ActionInvoked(self, nid, key):
        pass

    @dbus.service.signal("org.freedesktop.Notifications", signature="uu")
    def NotificationClosed(self, nid, reason):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--press-next", type=int, default=0)
    a = ap.parse_args()
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName("org.freedesktop.Notifications", bus)
    Server(bus, a.log, a.press_next)
    print("ready", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    sys.exit(main())
