"""Export this desktop's visible applications as the comparison's app fixture.

Writes one stripped .desktop file per application that GLib shows in the current
session, taken only from system folders (never ~/.local, which may hold personal
web apps). Only untranslated search and visibility keys are kept, and TryExec is
dropped, so every launcher in every container sees the same entries regardless
of locale or which programs are installed there.
"""
import argparse
import os
from pathlib import Path

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

ENTRY = "Desktop Entry"
KEPT_KEYS = ("Type", "Name", "GenericName", "X-GNOME-FullName", "Comment", "Keywords",
             "Exec", "Icon", "Categories", "OnlyShowIn", "NotShowIn", "StartupWMClass",
             "Terminal")


def system_entry(app):
    path = app.get_filename()
    return path is not None and not path.startswith(str(Path.home()))


def stripped(path):
    source = GLib.KeyFile()
    source.load_from_file(path, GLib.KeyFileFlags.NONE)
    present = set(source.get_keys(ENTRY)[0])
    lines = [f"[{ENTRY}]"]
    for key in KEPT_KEYS:
        if key in present:
            # get_value returns the untranslated, still-escaped value.
            lines.append(f"{key}={source.get_value(ENTRY, key)}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output", type=Path, help="fixture folder (gets applications/)")
    output = parser.parse_args().output / "applications"
    output.mkdir(parents=True, exist_ok=True)
    apps = [app for app in Gio.AppInfo.get_all() if app.should_show() and system_entry(app)]
    for app in sorted(apps, key=lambda app: app.get_id()):
        (output / app.get_id()).write_text(stripped(app.get_filename()))
    print(f"{len(apps)} applications for {os.environ.get('XDG_CURRENT_DESKTOP', '?')} "
          f"in {output}")


if __name__ == "__main__":
    main()
