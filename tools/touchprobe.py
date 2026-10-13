#!/usr/bin/env python3
"""Fullscreen GTK4 probe: logs where touches arrive in logical screen coordinates (run inside the GNOME session, e.g.
runuser -u ubuntu -- env WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus python3 touchprobe.py [seconds] [log]"""
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk  # noqa: E402

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 40
log = open(sys.argv[2] if len(sys.argv) > 2 else "/tmp/probe.log", "w")
area = None


def pressed(gesture, n, x, y):
    log.write("press x=%.1f y=%.1f  widget=%dx%d\n" % (x, y, area.get_width(), area.get_height()))
    log.flush()


def activate(app):
    global area
    win = Gtk.ApplicationWindow(application=app)
    area = Gtk.DrawingArea()
    area.set_hexpand(True)
    area.set_vexpand(True)
    g = Gtk.GestureClick()
    g.connect("pressed", pressed)
    area.add_controller(g)
    win.set_child(area)
    win.fullscreen()
    win.present()
    log.write("window up\n")
    log.flush()
    GLib.timeout_add_seconds(secs, app.quit)


app = Gtk.Application(application_id="tb330fu.touchprobe")
app.connect("activate", activate)
app.run([])
