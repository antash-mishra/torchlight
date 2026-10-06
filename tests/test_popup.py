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
from popup_support import PopupDriver, wait_for

parser = argparse.ArgumentParser()
parser.add_argument("--launcher", default="build/torchlight-gtk")
parser.add_argument("--daemon", default="build/torchlightd")
parser.add_argument("--xdotool", default="xdotool")
parser.add_argument("--capture")
parser.add_argument("--matrix", action="store_true", help="Repeat in light/dark/high-contrast GTK themes and scale overrides")
parser.add_argument("--a11y", action="store_true", help="Inspect the native AT-SPI tree and edit through accessibility")
parser.add_argument("--no-history", action="store_true")
parser.add_argument("--output", help="Write native-frame measurements as JSON")
args = parser.parse_args()
launcher = str(Path(args.launcher).resolve())
daemon_binary = str(Path(args.daemon).resolve())


if args.matrix:
    configurations = [
        ("light-100", dict(GTK_THEME="Adwaita", GDK_SCALE="1", GDK_DPI_SCALE="1")),
        ("dark-font-150", dict(GTK_THEME="Adwaita:dark", GDK_SCALE="1", GDK_DPI_SCALE="1.5")),
        ("contrast-200", dict(GTK_THEME="HighContrast", GDK_SCALE="2", GDK_DPI_SCALE="1")),
        ("pointer-on-panel", dict(GTK_THEME="Adwaita", GDK_SCALE="1", GDK_DPI_SCALE="1",
                                  TORCHLIGHT_TEST_PANEL="1")),
    ]
    measurements = []
    for label, overrides in configurations:
        command = [sys.executable, __file__, "--launcher", launcher, "--daemon", daemon_binary,
                   "--xdotool", args.xdotool]
        if args.capture:
            destination = Path(args.capture)
            command += ["--capture", str(destination.with_name(destination.stem + "-" + label + destination.suffix))]
        if args.a11y:
            command.append("--a11y")
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "metrics.json"
            print(label, flush=True)
            subprocess.run(command + ["--output", str(output)], env=dict(os.environ, **overrides), check=True)
            measurements.append(dict(configuration=label, **json.loads(output.read_text())))
    subprocess.run([sys.executable, __file__, "--launcher", launcher, "--daemon", daemon_binary,
                    "--xdotool", args.xdotool, "--no-history"], check=True)
    if args.output:
        Path(args.output).write_text(json.dumps(measurements, indent=2) + "\n")
    raise SystemExit(0)


with tempfile.TemporaryDirectory(prefix="torchlight-popup-") as directory:
    base = Path(directory)
    root = base / "files"
    root.mkdir()
    (root / "Keyboard notes.txt").touch()
    for index in range(12):
        (root / f"many-results-{index}.txt").touch()
    (root / ("longfilename-" + "x" * 220 + ".txt")).touch()
    raw_name = os.fsencode(root) + b"/oddbytes-\xff\n.txt"
    with open(raw_name, "wb"):
        pass
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
    file_marker = base / "file-accepted"
    opener = base / "xdg-open"
    opener.write_text(f"#!/bin/sh\nprintf '%s\\n' accepted >> '{file_marker}'\n")
    opener.chmod(0o700)
    for name, keywords in [("Display", "screen;resolution;"), ("Sound", "audio;volume;"), ("Keyboard", "typing;")]:
        (apps / f"{name.lower()}.desktop").write_text(
            f"[Desktop Entry]\nType=Application\nName={name}\nExec={executable}\n"
            f"Categories=Settings;\nKeywords={keywords}\nIcon=preferences-system-symbolic\n")
    broken = apps / "broken.desktop"
    broken.write_text(f"[Desktop Entry]\nType=Application\nName=BrokenLaunch\nExec={executable}\n"
                      f"Path={base / 'missing-working-directory'}\n")
    path = base / "torchlight.sock"
    database = base / "catalog.db"
    env = dict(os.environ, XDG_DATA_HOME=str(apps.parent), XDG_DATA_DIRS=str(base / "empty"),
               XDG_CURRENT_DESKTOP="X-Cinnamon", GSK_RENDERER="cairo",
               PATH=str(base) + os.pathsep + os.environ["PATH"])
    owner = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                            "--object-path", "/org/freedesktop/DBus", "--method",
                            "org.freedesktop.DBus.NameHasOwner", "org.torchlight.Launcher"],
                           env=env, capture_output=True, text=True, check=True)
    assert "true" not in owner.stdout, "An existing launcher owns this session; use test-ui-isolated"
    driver = PopupDriver(base, env, args.xdotool)
    driver.env["TORCHLIGHT_TEST_DELAY_OPEN"] = "1"
    daemon_log = tempfile.TemporaryFile()
    gui_log = tempfile.TemporaryFile()
    daemon_command = [daemon_binary, "--config", str(config), "--db", str(database), "--socket", str(path)]
    if args.no_history:
        daemon_command.append("--no-history")
    daemon = subprocess.Popen(daemon_command, env=env, stdout=daemon_log, stderr=daemon_log)
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
        return driver.xdo(*values, check=check)

    def focused():
        return driver.focused()

    def toggle():
        started = time.monotonic()
        subprocess.run([launcher, "--socket", str(path), "--toggle"], check=True, env=env,
                       stdout=gui_log, stderr=gui_log, timeout=8)
        wait_for(focused)
        return (time.monotonic() - started) * 1000

    def type_query(text):
        return driver.type_query(text)

    def empty_entry():
        driver.clear()
        xdo("key", "Return")
        time.sleep(.1)
        assert focused(), "Empty search must have no selected item to open"

    def assert_no_blank_searches():
        with sqlite3.connect(database) as db:
            queries = db.execute("SELECT query FROM searches").fetchall()
        assert all(query.strip() for (query,) in queries), queries

    try:
        wait_for(ready)
        screen_width, screen_height = map(int, xdo("getdisplaygeometry").split())
        if env.get("TORCHLIGHT_TEST_PANEL"):
            xdo("mousemove", screen_width // 2, 10)
        started = time.monotonic()
        gui = subprocess.Popen([launcher, "--socket", str(path), "--toggle"], env=driver.env,
                               stdout=gui_log, stderr=gui_log)
        wait_for(focused)
        first_focus = (time.monotonic() - started) * 1000
        first_frame = driver.frame(lambda f: f["first"])
        first_paint = first_frame["paint_us"] / 1000 - started * 1000
        window = xdo("getactivewindow")
        geometry = driver.geometry()
        assert geometry["WIDTH"] <= screen_width and geometry["HEIGHT"] <= screen_height, geometry
        assert abs(geometry["X"] - (screen_width - geometry["WIDTH"]) // 2) <= 2, geometry
        if env.get("TORCHLIGHT_TEST_PANEL"):
            assert abs(geometry["Y"] - (40 + (screen_height - 40) // 5)) <= 2, geometry
        empty_entry()
        idle = driver.read_frames()[-1]
        assert not idle["results_visible"] and not idle["footer_visible"] and not idle["clear_icon"], idle
        empty_geometry = driver.geometry()
        # Growth follows the fixed search anchor; typing never moves or narrows it.
        result = type_query("display")
        assert result["results_visible"] and result["footer_visible"] and result["typing"], result
        for key in ["entry_x", "entry_y", "entry_width"]:
            assert result[key] == idle[key], (key, idle, result)
        assert driver.geometry()["Y"] == empty_geometry["Y"], "Results moved window top"
        quiet = driver.frame(lambda f: f["query"] == "display" and not f["typing"], after=result["paint_us"] + 1)
        assert quiet["entry_y"] == idle["entry_y"]
        empty_entry()
        assert driver.geometry()["HEIGHT"] == empty_geometry["HEIGHT"], "Clearing did not shrink window"
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
        assert type_query("many-results")["rows"] == 10
        before = time.monotonic_ns() // 1000
        xdo("key", "--repeat", "12", "--delay", "0", "Down")
        driver.frame(lambda f: f["selected"] == 9, after=before)
        assert focused()
        assert driver.geometry()["HEIGHT"] <= screen_height * 7 // 10, driver.geometry()
        # Result labels must ellipsize even when the raw filename is very long
        # or contains invalid UTF-8/control bytes. They never become launch argv.
        assert type_query("longfilename")["rows"] >= 1
        assert driver.geometry()["WIDTH"] <= screen_width
        assert type_query("oddbytes")["rows"] >= 1
        assert driver.geometry()["WIDTH"] <= screen_width
        # An external opener has accepted the file, but its worker is deliberately
        # delayed. Escape and reopen a new query before completion is delivered.
        type_query("longfilename")
        xdo("key", "Return")
        wait_for(file_marker.exists)
        xdo("key", "Escape")
        wait_for(lambda: not focused())
        toggle()
        type_query("sound")
        time.sleep(.5)
        assert focused(), "An obsolete action must not dismiss a reopened popup"
        assert driver.read_frames()[-1]["query"] == "sound"
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT count(*) FROM opens").fetchone()[0] == (0 if args.no_history else 1)
        # A valid desktop entry with an invalid working directory fails native
        # activation synchronously, preserving both the query and popup focus.
        type_query("BrokenLaunch")
        before = time.monotonic_ns() // 1000
        xdo("key", "Return")
        driver.frame(lambda f: f["query"] == "BrokenLaunch" and f["status"] == "Could not open this item",
                     after=before)
        assert focused()
        broken.unlink()
        wait_for(lambda: not call("query", query="BrokenLaunch", limit=10)["results"])
        xdo("key", "Return")
        driver.frame(lambda f: f["query"] == "BrokenLaunch" and f["rows"] == 0 and f["ready"], after=before)
        assert focused()
        # Pasting beyond the protocol bound stays editable and recovers when
        # shortened; it must not masquerade as a daemon connection failure.
        before = time.monotonic_ns() // 1000
        xdo("key", "ctrl+a")
        xdo("type", "--clearmodifiers", "--delay", "0", "x" * 300)
        driver.frame(lambda f: "limited to 256 bytes" in f["status"] and len(f["query"]) == 300,
                     after=before)
        assert type_query("display")["rows"] >= 1
        if args.a11y:
            import gi
            gi.require_version("Atspi", "2.0")
            from gi.repository import Atspi

            def descendants(node):
                yield node
                for index in range(node.get_child_count()):
                    child = node.get_child_at_index(index)
                    if child is not None:
                        yield from descendants(child)

            tree = wait_for(lambda: next((node for node in descendants(Atspi.get_desktop(0))
                                          if node.get_role() == Atspi.Role.FRAME and node.get_name() == "Torchlight"), None))
            nodes = list(descendants(tree))
            editable = next(node for node in nodes if node.get_editable_text_iface() is not None)
            assert editable.get_name(), "Search entry must have an accessible name"
            before = time.monotonic_ns() // 1000
            assert editable.get_editable_text_iface().set_text_contents("sound")
            driver.frame(lambda f: f["query"] == "sound" and f["ready"] and f["rows"] > 0, after=before)
            nodes = list(descendants(tree))
            assert any(node.get_name() == "Sound" for node in nodes), [(node.get_role_name(), node.get_name()) for node in nodes]
            assert any(node.get_state_set().contains(Atspi.StateType.SELECTED) for node in nodes)
        # Test keyboard-only Retry with the service stopped, then reconnect
        # without restarting the popup when a new daemon instance is ready.
        daemon.terminate()
        daemon.wait(timeout=15)
        before = time.monotonic_ns() // 1000
        xdo("key", "ctrl+a")
        xdo("type", "--clearmodifiers", "--delay", "0", "display")
        failed = driver.frame(lambda f: f["retry"] and f["query"] == "display", after=before)
        xdo("key", "Return")
        input_retry = driver.frame(lambda f: f["retry_clicks"] > failed["retry_clicks"], timeout=2, after=before)
        for _ in range(6):
            xdo("key", "Tab")
            time.sleep(.05)
            if driver.read_frames()[-1]["retry_focus"]:
                break
        assert driver.read_frames()[-1]["retry_focus"], "Retry must be reachable by keyboard"
        xdo("key", "Return")
        driver.frame(lambda f: f["retry_clicks"] > input_retry["retry_clicks"], timeout=2, after=before)
        daemon = subprocess.Popen(daemon_command, env=env, stdout=daemon_log, stderr=daemon_log)
        wait_for(ready)
        driver.frame(lambda f: f["query"] == "display" and f["rows"] > 0 and f["ready"] and not f["retry"],
                     after=before)
        def history_count():
            with sqlite3.connect(database) as db:
                return db.execute("SELECT count(*) FROM desktop_opens").fetchone()[0]
        wait_for(lambda: history_count() == (0 if args.no_history else 5))
        empty_entry()
        assert_no_blank_searches()
        xdo("key", "Escape")
        from statistics import median
        measurements = dict(first_focus_ms=round(first_focus, 3), first_paint_ms=round(first_paint, 3),
                            repeat_focus_ms=round(repeat_focus, 3), geometry=geometry,
                            input_to_paint_ms=driver.input_ms, no_history=args.no_history,
                            accessibility=args.a11y)
        if args.output:
            Path(args.output).write_text(json.dumps(measurements, indent=2) + "\n")
        print(f"X11 popup passed: first paint {first_paint:.1f} ms, repeat focus {repeat_focus:.1f} ms, "
              f"median input-to-results paint {median(driver.input_ms):.1f} ms")
        print(geometry)
        print("Keyboard launches, empty/cleared queries, errors, restart/Retry, geometry and history passed.")
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
