"""Old-daemon/new-popup compatibility check in an isolated X11 session.

Accepts a pre-M9 daemon or an older v2 daemon without automatic suggestions.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time
from desktop_support import session_directory
from popup_support import PopupDriver, wait_for
import test_search_actions as fixture

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--daemon", required=True)
parser.add_argument("--typed", action="store_true", help="Daemon supports v2, without suggestions")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
fixture.DAEMON = str(Path(args.daemon).resolve())
launcher = str(Path("build/torchlight-gtk").resolve())
with session_directory(prefix="torchlight-popup-compat-") as directory:
    base = Path(directory)
    if args.typed:
        fixture.fake_flatpak(base)
    service = fixture.Service(base)
    (service.root / "needle.txt").touch()
    (service.root / "web literal.txt").touch()
    driver = PopupDriver(base, service.env, "xdotool")
    service.start()
    gui = None
    with (base / "gui.log").open("w") as log:
        try:
            capabilities = service.call("status").get("capabilities", {})
            assert "software_suggestions" not in capabilities
            assert bool(capabilities.get("search_actions")) == args.typed
            wait_for(lambda: len(service.call("query", query="needle")["results"]) == 1)
            gui = subprocess.Popen([launcher, "--socket", str(service.path), "--toggle"],
                                   env=driver.env, stdout=log, stderr=log)
            wait_for(driver.focused)
            assert driver.type_query("needle")["rows"] == (2 if args.typed else 1)
            if args.typed:
                assert driver.type_query("web cats")["rows"] == 1
                wait_for(lambda: service.call("status")["software"]["state"] == "ready")
                assert driver.type_query("install editor")["rows"] == 3
            else:
                since = time.monotonic_ns() // 1000
                driver.type_query("web cats", measure=False)
                driver.frame(lambda f: f["rows"] == 0 and "updated Torchlight service" in f["status"],
                             after=since)
                since = time.monotonic_ns() // 1000
                driver.type_query("install editor", measure=False)
                driver.frame(lambda f: f["rows"] == 0 and "updated Torchlight service" in f["status"],
                             after=since)
            assert driver.type_query("local web literal")["rows"] == 1
            args.output.write_text(json.dumps(dict(old_daemon=fixture.DAEMON,
                typed=args.typed, local=True, web=True, software=True, prefix_escape=True), indent=2))
            print("New popup / old daemon: local search, explicit-mode guidance and escape passed.")
        finally:
            if gui is not None:
                gui.terminate()
                gui.wait(timeout=10)
            service.stop()
