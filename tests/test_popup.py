"""GTK X11 acceptance using an isolated daemon, fixture apps and real keyboard input.

Run in a desktop session or Xvfb with a window manager and session bus. Never
changes desktop shortcuts or launches real user applications. Optional screenshot
uses the screenshot skill helper only when explicitly requested by --capture.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--launcher", default="build/torchlight-gtk")
parser.add_argument("--daemon", default="build/torchlightd")
parser.add_argument("--xdotool", default="xdotool")
parser.add_argument("--capture")
parser.add_argument("--matrix", action="store_true", help="Repeat in light/dark/high-contrast GTK themes and scale overrides")
args = parser.parse_args()
launcher = str(Path(args.launcher).resolve())
daemon_binary = str(Path(args.daemon).resolve())


def wait_for(predicate, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(.02)
    raise AssertionError("condition timed out")


if args.matrix:
    configurations = [
        ("light-100", dict(GTK_THEME="Adwaita", GDK_SCALE="1", GDK_DPI_SCALE="1")),
        ("dark-font-150", dict(GTK_THEME="Adwaita:dark", GDK_SCALE="1", GDK_DPI_SCALE="1.5")),
        ("contrast-200", dict(GTK_THEME="HighContrast", GDK_SCALE="2", GDK_DPI_SCALE="1")),
    ]
    for label, overrides in configurations:
        command = [sys.executable, __file__, "--launcher", launcher, "--daemon", daemon_binary,
                   "--xdotool", args.xdotool]
        if args.capture:
            destination = Path(args.capture)
            command += ["--capture", str(destination.with_name(destination.stem + "-" + label + destination.suffix))]
        print(label, flush=True)
        subprocess.run(command, env=dict(os.environ, **overrides), check=True)
    raise SystemExit(0)


with tempfile.TemporaryDirectory(prefix="torchlight-popup-") as directory:
    base = Path(directory)
    root = base / "files"
    root.mkdir()
    (root / "Keyboard notes.txt").touch()
    config = base / "config"
    config.write_text(f"root = {root}\n")
    apps = base / "data" / "applications"
    apps.mkdir(parents=True)
    (apps.parent / "mime").symlink_to("/usr/share/mime", target_is_directory=True)
    (apps.parent / "icons").symlink_to("/usr/share/icons", target_is_directory=True)
    marker = base / "accepted"
    executable = base / "launch-fixture"
    executable.write_text(f"#!/bin/sh\nprintf '%s\\n' accepted >> '{marker}'\n")
    executable.chmod(0o700)
    for name, keywords in [("Display", "screen;resolution;"), ("Sound", "audio;volume;"), ("Keyboard", "typing;")]:
        (apps / f"{name.lower()}.desktop").write_text(
            f"[Desktop Entry]\nType=Application\nName={name}\nExec={executable}\n"
            f"Categories=Settings;\nKeywords={keywords}\nIcon=preferences-system-symbolic\n")
    path = base / "torchlight.sock"
    database = base / "catalog.db"
    env = dict(os.environ, XDG_DATA_HOME=str(apps.parent), XDG_DATA_DIRS=str(base / "empty"),
               XDG_CURRENT_DESKTOP="X-Cinnamon", GSK_RENDERER="cairo")
    daemon_log = tempfile.TemporaryFile()
    gui_log = tempfile.TemporaryFile()
    daemon = subprocess.Popen([daemon_binary, "--config", str(config), "--db", str(database),
                               "--socket", str(path)], env=env, stdout=daemon_log, stderr=daemon_log)
    gui = None

    def call(op, **fields):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(8)
            connection.connect(str(path))
            connection.sendall(json.dumps(dict(version=1, request_id="probe", op=op, **fields)).encode() + b"\n")
            with connection.makefile("rb") as stream:
                return json.loads(stream.readline(1024 * 1024))

    def ready():
        assert daemon.poll() is None
        try:
            return call("status")["status"] == "ok"
        except OSError:
            return False

    def xdo(*values, check=True):
        result = subprocess.run([args.xdotool, *map(str, values)], capture_output=True, text=True,
                                check=check, env=env)
        return result.stdout.strip()

    def focused():
        try:
            return xdo("getwindowname", xdo("getactivewindow")) == "Torchlight"
        except subprocess.CalledProcessError:
            return False

    def toggle():
        started = time.monotonic()
        subprocess.run([launcher, "--socket", str(path), "--toggle"], check=True, env=env,
                       stdout=gui_log, stderr=gui_log, timeout=8)
        wait_for(focused)
        return (time.monotonic() - started) * 1000

    def type_query(text):
        xdo("key", "ctrl+a")
        xdo("type", "--clearmodifiers", "--delay", "1", text)
        time.sleep(.25)

    def empty_entry():
        xdo("key", "ctrl+a", "BackSpace")
        time.sleep(.25)
        xdo("key", "Return")
        time.sleep(.1)
        assert focused(), "Empty search must have no selected item to open"

    def assert_no_blank_searches():
        with sqlite3.connect(database) as db:
            queries = db.execute("SELECT query FROM searches").fetchall()
        assert all(query.strip() for (query,) in queries), queries

    try:
        wait_for(ready)
        started = time.monotonic()
        gui = subprocess.Popen([launcher, "--socket", str(path), "--toggle"], env=env,
                               stdout=gui_log, stderr=gui_log)
        wait_for(focused)
        first_focus = (time.monotonic() - started) * 1000
        window = xdo("getactivewindow")
        geometry = xdo("getwindowgeometry", "--shell", window)
        empty_entry()
        assert_no_blank_searches()
        # Escape dismisses; the next invocation reuses the first process/window.
        xdo("key", "Escape")
        wait_for(lambda: not focused())
        repeat_focus = toggle()
        assert gui.poll() is None
        assert xdo("getactivewindow") == window
        # Clearing existing results and whitespace-only input must be inert.
        type_query("display")
        empty_entry()
        type_query("   ")
        xdo("key", "Return")
        time.sleep(.1)
        assert focused() and not marker.exists()
        empty_entry()
        # Clear immediately after typing to exercise pending response cancellation.
        xdo("type", "--clearmodifiers", "--delay", "0", "screen")
        empty_entry()
        for text in ["display", "screen", "resolution", "sound", "keyboard"]:
            type_query(text)
            before = len(marker.read_text().splitlines()) if marker.exists() else 0
            xdo("key", "Return")
            wait_for(lambda: marker.exists() and len(marker.read_text().splitlines()) == before + 1)
            wait_for(lambda: not focused())
            toggle()
        type_query("s")
        xdo("type", "--clearmodifiers", "--delay", "0", "creen")
        time.sleep(.2)
        if args.capture:
            subprocess.run(["python3", "/home/antash/.codex/skills/screenshot/scripts/take_screenshot.py",
                            "--active-window", "--path", args.capture], check=True, env=os.environ)
        # Toggle while focused hides, and then shows the same instance.
        subprocess.run([launcher, "--toggle"], env=env, check=True, stdout=gui_log, stderr=gui_log)
        wait_for(lambda: not focused())
        toggle()
        def history_count():
            with sqlite3.connect(database) as db:
                return db.execute("SELECT count(*) FROM desktop_opens").fetchone()[0]
        wait_for(lambda: history_count() == 5)
        empty_entry()
        assert_no_blank_searches()
        xdo("key", "Escape")
        print(f"Cinnamon/X11 popup passed: first focus {first_focus:.1f} ms, repeat focus {repeat_focus:.1f} ms")
        print(geometry)
        print("Empty/cleared searches, keyboard launches, toggle and history passed.")
    finally:
        if gui is not None:
            gui.terminate()
            gui.wait(timeout=8)
        daemon.terminate()
        daemon.wait(timeout=15)
        gui_log.seek(0)
        output = gui_log.read().decode(errors="replace")
        assert "CRITICAL" not in output and "ERROR" not in output and "Gtk-WARNING" not in output, output
        daemon_log.close()
        gui_log.close()
