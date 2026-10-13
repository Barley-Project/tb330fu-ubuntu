#!/usr/bin/env python3
"""GTK4 frame-rate probe: counts frame-clock ticks (= compositor frame callbacks) of a fullscreen window while something animates.
mode "small": a progress bar moves (small damage per frame, like normal UI use); mode "full": the whole window changes colour every frame (full-screen damage).
runuser -u ubuntu -- env WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus GTK_A11Y=none python3 fpsprobe.py [small|full] [seconds]"""
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

mode = sys.argv[1] if len(sys.argv) > 1 else "small"
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 8
ticks = []
css = Gtk.CssProvider()
state = {"i": 0}


def tick(widget, clock):
    ticks.append(time.monotonic())
    state["i"] += 1
    if mode == "small":
        bar.set_fraction((state["i"] % 200) / 200.0)
    else:
        c = state["i"] % 2
        css.load_from_string("window.probe { background: %s; }" % ("#e0301e" if c else "#1e30e0"))
    return GLib.SOURCE_CONTINUE


def done(app):
    if len(ticks) > 10:
        iv = [b - a for a, b in zip(ticks, ticks[1:])]
        span = ticks[-1] - ticks[0]
        print("%s: frames %d in %.2f s = %.1f fps; interval min %.1f ms, median %.1f ms, max %.1f ms" %
              (mode, len(ticks), span, (len(ticks) - 1) / span, min(iv) * 1000, sorted(iv)[len(iv) // 2] * 1000, max(iv) * 1000), flush=True)
    app.quit()
    return GLib.SOURCE_REMOVE


def activate(app):
    global bar
    win = Gtk.ApplicationWindow(application=app)
    win.add_css_class("probe")
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    bar = Gtk.ProgressBar()
    bar.set_valign(Gtk.Align.CENTER)
    bar.set_margin_start(100)
    bar.set_margin_end(100)
    win.set_child(bar)
    win.add_tick_callback(tick)
    win.fullscreen()
    win.present()
    GLib.timeout_add_seconds(secs, done, app)


bar = None
app = Gtk.Application(application_id="tb330fu.fpsprobe")
app.connect("activate", activate)
app.run([])
