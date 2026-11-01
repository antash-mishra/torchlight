"""Idle-exit integration against a real user systemd manager, on a private display.

Run in Boat with Xvfb/Metacity and the VM's user bus (not dbus-run-session).
Only uniquely named fixture units and their own children are stopped.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
from desktop_support import session_directory
from popup_support import PopupDriver, wait_for
from test_search_actions import Service

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--launcher", default="build/release/torchlight-gtk")
parser.add_argument("--output", required=True)
args = parser.parse_args()
launcher = str(Path(args.launcher).resolve())
results = []

with session_directory(prefix="torchlight-idle-service-") as directory:
    base = Path(directory)
    service = Service(base, "software_search = false\n")
    marker = base / "child.pid"
    child = base / "child.py"
    child.write_text(f"import os, pathlib, time\npathlib.Path({str(marker)!r}).write_text(str(os.getpid()))\ntime.sleep(120)\n")
    # A raw file opener stays in the popup's cgroup, exposing service teardown.
    (service.root / "surviving-file.txt").touch()
    programs = base / "bin"
    programs.mkdir()
    opener = programs / "xdg-open"
    opener.write_text(f"#!/bin/sh\nexec {sys.executable} '{child}'\n")
    opener.chmod(0o700)
    # The fixture changes XDG_RUNTIME_DIR; retain the actual manager's bus address.
    bus = os.environ.get("DBUS_SESSION_BUS_ADDRESS",
                         f"unix:path=/run/user/{os.getuid()}/bus")
    driver = PopupDriver(base, dict(service.env, DBUS_SESSION_BUS_ADDRESS=bus), "xdotool")
    service.start()
    try:
        for exit_type, restart, should_exit in [("main", "no", False),
                                                ("cgroup", "always", False),
                                                ("cgroup", "no", True)]:
            unit = "torchlight-idle-test-" + uuid.uuid4().hex + ".service"
            environment = [f"{k}={v}" for k, v in driver.env.items()]
            command = ["systemd-run", "--user", "--quiet", "--collect", "--unit", unit,
                       "--property=Type=exec", "--property=ExitType=" + exit_type,
                       "--property=Restart=" + restart, "env", *environment, launcher,
                       "--socket", str(service.path), "--idle-exit-seconds=2", "--toggle"]
            try:
                subprocess.run(command, check=True, timeout=10)
                wait_for(driver.focused)
                pid = int(subprocess.check_output(["systemctl", "--user", "show", unit,
                                                   "--property=MainPID", "--value"]))
                driver.type_query("surviving-file")
                driver.xdo("key", "Return")
                wait_for(marker.exists)
                child_pid = int(marker.read_text())
                wait_for(lambda: not driver.focused())
                time.sleep(5)
                exited = not Path(f"/proc/{pid}").exists()
                assert exited == should_exit, (unit, pid, exited, should_exit)
                os.kill(child_pid, 0)
                properties = subprocess.check_output(["systemctl", "--user", "show", unit,
                    "--property=NRestarts", "--property=ActiveState"], text=True)
                assert "NRestarts=0" in properties and "ActiveState=active" in properties, properties
                results.append(dict(exit_type=exit_type, restart=restart, popup_exited=exited,
                                    child_survived=True, restarts=0))
            finally:
                subprocess.run(["systemctl", "--user", "stop", unit], check=True, timeout=15)
                if marker.exists():
                    marker.unlink()
        Path(args.output).write_text(json.dumps(results, indent=2) + "\n")
        print(json.dumps(results, indent=2))
    finally:
        service.stop()
