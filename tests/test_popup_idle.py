"""Native idle lifecycle, cancellation and child-survival regressions on a private bus."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from desktop_support import session_directory
from popup_support import PopupDriver, wait_for
from test_search_actions import Service, fake_flatpak

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--launcher", default="build/torchlight-gtk")
parser.add_argument("--xdotool", default="xdotool")
parser.add_argument("--output")
args = parser.parse_args()
launcher = str(Path(args.launcher).resolve())

with session_directory(prefix="torchlight-idle-") as directory:
    base = Path(directory)
    fake_flatpak(base)
    service = Service(base)
    (service.root / "idle-file.txt").touch()
    apps = base / "data/applications"
    apps.mkdir(parents=True)
    config = base / "config-home"
    config.mkdir()
    child = base / "child.py"
    child.write_text("import os, pathlib, sys, time\n"
                     "pathlib.Path(sys.argv[1]).write_text(str(os.getpid()))\n"
                     "time.sleep(120)\n")
    markers = {kind: base / (kind + ".pid") for kind in ["application", "browser", "software", "file"]}
    for kind, name in [("software", "gnome-software"), ("file", "xdg-open")]:
        program = base / "bin" / name
        program.write_text(f"#!/bin/sh\nexec {sys.executable} '{child}' '{markers[kind]}'\n")
        program.chmod(0o700)
    for kind, name in [("application", "Idle Fixture Application"), ("browser", "Fixture Browser")]:
        (apps / (kind + ".desktop")).write_text(
            f"[Desktop Entry]\nType=Application\nName={name}\n"
            f"Exec={sys.executable} {child} {markers[kind]}\n"
            + ("NoDisplay=true\nMimeType=x-scheme-handler/https;\n" if kind == "browser" else ""))
    (config / "mimeapps.list").write_text(
        "[Default Applications]\nx-scheme-handler/https=browser.desktop;\n")
    driver = PopupDriver(base, dict(service.env, GTK_USE_PORTAL="0"), args.xdotool)
    gui = None
    invocations = []
    children = []
    checks = []
    service.start()
    with (base / "popup.log").open("w+") as log:
        def start(timeout=2):
            process = subprocess.Popen([launcher, "--socket", str(service.path),
                                        "--idle-exit-seconds", str(timeout), "--toggle"],
                                       env=driver.env, stdout=log, stderr=log)
            wait_for(driver.focused)
            return process

        def hidden_exit():
            wait_for(lambda: not driver.focused())
            assert gui.wait(timeout=7) == 0

        try:
            wait_for(lambda: service.call("status")["software"]["state"] == "ready")
            for invalid in ["-1", "86401", "invalid"]:
                result = subprocess.run([launcher, "--idle-exit-seconds", invalid],
                                        env=driver.env, stdout=log, stderr=log, timeout=8)
                assert result.returncode != 0
            checks.append("invalid timeout rejected")
            gui = start()
            initial = driver.type_query("idle-file")
            time.sleep(3.2)
            assert gui.poll() is None and driver.focused()
            driver.xdo("key", "Escape")
            wait_for(lambda: not driver.focused())
            time.sleep(.3)
            subprocess.run([launcher, "--toggle"], env=driver.env, stdout=log,
                           stderr=log, check=True, timeout=8)
            wait_for(driver.focused)
            time.sleep(3.2)
            assert gui.poll() is None and driver.focused()
            checks.extend(["visible popup stays alive", "quick reopen cancels idle exit"])
            driver.xdo("key", "Escape")
            hidden_exit()
            checks.append("hidden popup exits cleanly")
            gui = start()
            assert driver.type_query("idle-file")["rows"] == initial["rows"]
            driver.xdo("key", "Escape")
            hidden_exit()
            checks.append("new process restores search")
            for kind, query in [("application", "Idle Fixture Application"),
                                ("browser", "https://example.com"),
                                ("software", "Example Editor"), ("file", "idle-file")]:
                gui = start()
                driver.type_query(query)
                driver.xdo("key", "Return")
                wait_for(markers[kind].exists)
                pid = int(markers[kind].read_text())
                children.append(pid)
                hidden_exit()
                os.kill(pid, 0)
                assert service.call("status")["status"] == "ok"
                checks.append(kind + " remains running after popup exit")
            gui = start(timeout=0)
            driver.xdo("key", "Escape")
            time.sleep(3.2)
            assert gui.poll() is None
            checks.append("resident option keeps hidden process")
            gui.terminate()
            gui.wait(timeout=10)
            gui = start()
            # A shortcut may arrive on either side of the idle deadline: it must
            # reach the surviving instance or start its replacement, without a lost press.
            for pause in [1.8, 2.0, 2.2, 2.5, 1.9, 2.1]:
                driver.xdo("key", "Escape")
                wait_for(lambda: not driver.focused())
                time.sleep(pause)
                invoked = subprocess.Popen([launcher, "--socket", str(service.path),
                                            "--idle-exit-seconds=2", "--toggle"],
                                           env=driver.env, stdout=log, stderr=log)
                invocations.append(invoked)
                wait_for(driver.focused)
                assert driver.type_query("idle-file")["rows"] == initial["rows"]
                if gui.poll() is not None:
                    gui = invoked
            checks.append("shortcut works across idle deadline")
            result = dict(checks=checks, passed=True)
            print(json.dumps(result, indent=2))
            if args.output:
                Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
        finally:
            if gui is not None and gui.poll() is None:
                gui.terminate()
                gui.wait(timeout=10)
            for process in invocations:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
            for pid in children:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            service.stop()
            log.seek(0)
            errors = log.read()
            assert "Gtk-CRITICAL" not in errors and "GLib-CRITICAL" not in errors, errors
