"""Real GTK keyboard acceptance for typed actions, using inert external fixtures."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time
from desktop_support import session_directory
from popup_support import PopupDriver, wait_for
from test_search_actions import Service, fake_flatpak

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--xdotool", default="xdotool")
parser.add_argument("--output")
parser.add_argument("--launcher", default="build/torchlight-gtk")
args = parser.parse_args()
launcher = str(Path(args.launcher).resolve())

with session_directory(prefix="torchlight-popup-actions-") as directory:
    base = Path(directory)
    fake_flatpak(base)
    service = Service(base)
    for i in range(20):
        (service.root / f"needle-{i:02}.txt").touch()
    (service.root / "install notes").touch()
    (service.root / "report.pdf").touch()
    (service.root / "google.com-notes.txt").touch()
    for i in range(10):
        (service.root / f"Example Editor doc-{i:02}.txt").touch()
    apps = base / "data/applications"
    apps.mkdir(parents=True)
    config = base / "config-home"
    config.mkdir()
    browser = base / "bin/browser"
    browser_marker = base / "browser-accepted.log"
    browser.write_text(f"#!/bin/sh\nprintf '%s' \"$1\" > '{browser_marker}'\n")
    browser.chmod(0o700)
    (apps / "browser.desktop").write_text(
        f"[Desktop Entry]\nType=Application\nName=Test Browser\nNoDisplay=true\n"
        f"Exec={browser} %u\nMimeType=x-scheme-handler/https;\n")
    (config / "mimeapps.list").write_text(
        "[Default Applications]\nx-scheme-handler/https=browser.desktop;\n")
    manager = base / "bin/gnome-software"
    manager_marker = base / "software-accepted.log"
    manager.write_text(f"#!/bin/sh\nprintf '%s' \"$1\" > '{manager_marker}'\n")
    manager.chmod(0o700)
    service.env.update(XDG_CURRENT_DESKTOP="X-Cinnamon", GTK_USE_PORTAL="0")
    driver = PopupDriver(base, service.env, args.xdotool)
    # Only inert fixture executables are available to the GUI action worker.
    gui_env = dict(driver.env, PATH=str(base / "bin"))
    service.start()
    gui = None
    with (base / "gui.log").open("w") as log:
        try:
            wait_for(lambda: service.call("status")["software"]["state"] == "ready")
            wait_for(lambda: len(service.call("query", query="needle", limit=10)["results"]) == 10)
            gui = subprocess.Popen([launcher, "--socket", str(service.path), "--toggle"],
                                   env=gui_env, stdout=log, stderr=log)
            wait_for(driver.focused)

            def show():
                if not driver.focused():
                    subprocess.run([launcher, "--socket", str(service.path), "--toggle"],
                                   env=gui_env, check=True, stdout=log, stderr=log, timeout=8)
                    wait_for(driver.focused)

            automatic = driver.type_query("Example Editor")
            assert automatic["rows"] == 12 and automatic["selected"] == 1 and automatic["install_selected"], automatic
            assert not manager_marker.exists(), "Typing launched Software"
            driver.xdo("key", "Return")
            wait_for(manager_marker.exists)
            assert manager_marker.read_text() == "--details=org.example.Other"
            wait_for(lambda: not driver.focused())
            manager_marker.unlink()
            show()
            assert driver.type_query("local Example Editor")["rows"] == 10
            exact_file = service.root / "Example Editor"
            exact_file.touch()
            wait_for(lambda: any(r.get("path") == str(exact_file)
                                for r in service.call("query", query="Example Editor")["results"]))
            local = driver.type_query("Example Editor")
            assert local["selected"] == 1 and not local["install_selected"], local
            exact_file.unlink()

            ordinary = driver.type_query("needle")
            assert ordinary["rows"] == 11 and ordinary["selected"] == 1 and ordinary["web_visible"]
            since = time.monotonic_ns() // 1000
            driver.xdo("key", "--repeat", "15", "--delay", "0", "Down")
            scrolled = driver.frame(lambda f: f["selected"] == 10, after=since)
            assert scrolled["web_visible"] and scrolled["web_y"] == ordinary["web_y"]
            driver.xdo("key", "alt+Return")
            wait_for(browser_marker.exists)
            assert browser_marker.read_text() == "https://duckduckgo.com/?q=needle"
            wait_for(lambda: not driver.focused())
            browser_marker.unlink()
            show()
            address = driver.type_query("google.com")
            assert address["selected"] == 0 and address["web_visible"]
            driver.xdo("key", "Return")
            wait_for(browser_marker.exists)
            assert browser_marker.read_text() == "https://google.com"
            wait_for(lambda: not driver.focused())
            browser_marker.unlink()
            show()
            assert driver.type_query("report.pdf")["selected"] == 1
            assert driver.type_query("local needle")["rows"] == 10
            assert driver.type_query("local install notes")["rows"] == 1
            assert driver.type_query("web cats & dogs")["rows"] == 1
            assert not browser_marker.exists(), "Typing launched a browser"
            driver.xdo("key", "ctrl+Return")
            time.sleep(0.1)
            assert not browser_marker.exists(), "Reveal acted on a Web target"
            driver.xdo("key", "Return")
            wait_for(browser_marker.exists)
            assert browser_marker.read_text() == "https://duckduckgo.com/?q=cats%20%26%20dogs"
            wait_for(lambda: not driver.focused())
            show()
            browser_marker.unlink()
            assert driver.type_query("needle")["selected"] == 1
            assert not driver.read_frames()[-1]["preferences_present"]
            # Retain the engine, but ignore legacy browser overrides for every user.
            preferences = config / "torchlight/web.ini"
            preferences.parent.mkdir(exist_ok=True)
            saved_preferences = "[Web]\nengine=google\nbrowser=google-chrome.desktop\n"
            preferences.write_text(saved_preferences)
            gui.terminate()
            gui.wait(timeout=10)
            gui = subprocess.Popen([launcher, "--socket", str(service.path), "--toggle"],
                                   env=gui_env, stdout=log, stderr=log)
            wait_for(driver.focused)
            assert driver.type_query("needle")["selected"] == 1
            assert not driver.read_frames()[-1]["preferences_present"]
            # Switching applications still dismisses the launcher.
            popup_window = driver.xdo("getactivewindow")
            with subprocess.Popen([str(Path("build/test_window_app").resolve()),
                                   "focus-test", "FocusTest", "Other application"],
                                  env=driver.env, stdout=subprocess.PIPE, text=True) as other_window:
                try:
                    external_window = other_window.stdout.readline().strip()
                    driver.xdo("windowactivate", "--sync", external_window)
                    wait_for(lambda: not driver.focused())
                    wait_for(lambda: popup_window not in driver.xdo(
                        "search", "--onlyvisible", "--name", "^Torchlight$",
                        check=False).splitlines())
                finally:
                    other_window.terminate()
                    other_window.wait(timeout=8)
            show()
            assert driver.type_query("web saved choice")["rows"] == 1
            driver.xdo("key", "Return")
            wait_for(browser_marker.exists)
            assert browser_marker.read_text() == "https://www.google.com/search?q=saved%20choice"
            wait_for(lambda: not driver.focused())
            browser_marker.unlink()
            assert preferences.read_text() == saved_preferences
            # Changing the desktop default takes effect without restarting the popup.
            other_marker = base / "other-browser.log"
            other = base / "bin/other-browser"
            other.write_text(f"#!/bin/sh\nprintf '%s' \"$1\" > '{other_marker}'\n")
            other.chmod(0o700)
            (apps / "other.desktop").write_text(
                f"[Desktop Entry]\nType=Application\nName=Other Browser\nNoDisplay=true\n"
                f"Exec={other} %u\nMimeType=x-scheme-handler/https;\n")
            (config / "mimeapps.list").write_text(
                "[Default Applications]\nx-scheme-handler/https=other.desktop;\n")
            show()
            assert driver.type_query("google.com")["selected"] == 0
            driver.xdo("key", "alt+Return")
            wait_for(other_marker.exists)
            assert other_marker.read_text() == "https://www.google.com/search?q=google.com"
            assert not browser_marker.exists(), "Web search kept the previous default browser"
            wait_for(lambda: not driver.focused())
            show()
            other_marker.unlink()
            address = driver.type_query("google.com")
            window = driver.xdo("getactivewindow")
            driver.xdo("mousemove", "--window", window, driver.geometry()["WIDTH"] // 2,
                       int(address["web_y"] + address["web_height"] / 2), "click", "1")
            wait_for(other_marker.exists)
            assert other_marker.read_text() == "https://google.com"
            assert not browser_marker.exists(), "Website activation kept the previous default"
            wait_for(lambda: not driver.focused())
            other_marker.unlink()
            # Engine-only settings work too; no browser key is required after upgrade.
            preferences.write_text("[Web]\nengine=google\n")
            gui.terminate()
            gui.wait(timeout=10)
            gui = subprocess.Popen([launcher, "--socket", str(service.path), "--toggle"],
                                   env=gui_env, stdout=log, stderr=log)
            wait_for(driver.focused)
            assert driver.type_query("web engine only")["rows"] == 1
            driver.xdo("key", "Return")
            wait_for(other_marker.exists)
            assert other_marker.read_text() == "https://www.google.com/search?q=engine%20only"
            wait_for(lambda: not driver.focused())
            other_marker.unlink()
            show()
            # No installed handler leaves the query visible and reports an error.
            browser.unlink()
            other.unlink()
            assert driver.type_query("web missing browser")["rows"] == 1
            since = time.monotonic_ns() // 1000
            driver.xdo("key", "Return")
            driver.frame(lambda f: bool(f["status"]) and "Searching" not in f["status"],
                         after=since)
            assert driver.focused() and not browser_marker.exists() and not other_marker.exists()
            assert driver.type_query("install editor")["rows"] == 3
            assert not manager_marker.exists(), "Typing launched Software"
            driver.xdo("key", "Return")
            wait_for(manager_marker.exists)
            assert manager_marker.read_text() == "--details=org.example.Editor"
            wait_for(lambda: not driver.focused())
            # Missing managers leave the query and selection available for retry.
            manager.unlink()
            show()
            assert driver.type_query("install editor")["rows"] == 3
            since = time.monotonic_ns() // 1000
            driver.xdo("key", "Return")
            driver.frame(lambda f: f["status"].startswith("No supported software manager"), after=since)
            assert driver.focused()
            # Changing the selected source after it was displayed must block handoff.
            source = base / "flatpak-user/repo/config"
            source.write_text(source.read_text().replace("/one", "/replacement"))
            since = time.monotonic_ns() // 1000
            driver.xdo("key", "Return")
            driver.frame(lambda f: "source changed" in f["status"] or "no longer indexed" in f["status"],
                         after=since)
            assert driver.focused()
            driver.type_query("web ", measure=False)
            driver.frame(lambda f: f["query"] == "web " and f["rows"] == 0 and "Type a web search" in f["status"])
            # Browser/Software handoffs never become local open history.
            with sqlite3.connect(service.base / "catalog.db") as database:
                assert database.execute("SELECT count(*) FROM opens").fetchone()[0] == 0
                assert database.execute("SELECT count(*) FROM desktop_opens").fetchone()[0] == 0
                assert database.execute("SELECT count(*) FROM searches").fetchone()[0] == 0
            # A native installation is opened from the same ordinary query.
            # Its desktop id intentionally differs from the Flatpak app id.
            native_marker = base / "native-opened.log"
            native = base / "bin/native-editor"
            native.write_text(f"#!/bin/sh\nprintf opened > '{native_marker}'\n")
            native.chmod(0o700)
            (apps / "native-editor.desktop").write_text(
                f"[Desktop Entry]\nType=Application\nName=Example Editor\nExec={native}\n")
            wait_for(lambda: any(r.get("desktop_id") == "native-editor.desktop"
                                for r in service.call("query", query="Example Editor")["results"]))
            installed = driver.type_query("Example Editor")
            assert installed["rows"] == 11 and installed["selected"] == 1 and not installed["install_selected"], installed
            driver.xdo("key", "Return")
            wait_for(native_marker.exists)
            wait_for(lambda: not driver.focused())
            result = dict(status="passed", local_slots=10, web_slots=1, software_variants=3,
                          browser_handoff=True, software_handoff=True, stale_source=True,
                          pinned_web=True, direct_address=True, preferences_loaded=True, gear_removed=True, alt_enter=True,
                          legacy_browser_ignored=True, default_browser_changed_live=True, engine_only=True,
                          focus_dismissal=True, web_pointer=True,
                          automatic_install=True, exact_file_priority=True, native_app_open=True,
                          no_typed_history=True)
            (base / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            if args.output:
                Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
        finally:
            if gui is not None:
                gui.terminate()
                gui.wait(timeout=10)
            service.stop()
print("Typed popup actions passed: slots, prefixes, browser, Software, source changes and history.")
