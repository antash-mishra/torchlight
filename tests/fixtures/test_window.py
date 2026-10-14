"""A GTK 4 test window with a given application id and title, for window listing checks on
Wayland, where the application id is the window's identity. Runs until killed.

usage: test_window.py APPLICATION_ID TITLE
"""
import sys

import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gio, Gtk  # noqa: E402

application_id, title = sys.argv[1], sys.argv[2]
# NON_UNIQUE: two windows of the same id come from two processes, like two app instances.
application = Gtk.Application(application_id=application_id, flags=Gio.ApplicationFlags.NON_UNIQUE)


def activate(app):
    window = Gtk.ApplicationWindow(application=app, title=title)
    window.set_default_size(320, 200)
    window.present()


application.connect("activate", activate)
raise SystemExit(application.run([sys.argv[0]]))
