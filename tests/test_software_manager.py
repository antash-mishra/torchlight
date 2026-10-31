"""Explicit Boat-only manager acceptance: details first, optional confirmed install.

Run inside a private desktop/session bus with configured user Flatpak fixtures.
On a cloud VM with no NetworkManager connectivity, use GIO_USE_NETWORK_MONITOR=base.
Never part of make test: --install clicks the manager's Install button.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time
import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--manager", choices=("gnome", "kde"), required=True)
parser.add_argument("--app-id", required=True)
parser.add_argument("--name", required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--install", action="store_true")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)


def descendants(root):
    queue = [root]
    count = 0
    while queue and count < 3000:
        node = queue.pop(0)
        if node is None:
            continue
        count += 1
        yield node
        try:
            queue.extend(node.get_child_at_index(i) for i in range(node.get_child_count()))
        except Exception:
            continue


def installed():
    return subprocess.run(["flatpak", "--user", "info", args.app_id],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          timeout=10).returncode == 0


def capture(name):
    subprocess.run(["import", "-window", "root", str(args.output / f"{name}.png")],
                   check=True, timeout=10)


command = (["gnome-software", f"--details={args.app_id}"] if args.manager == "gnome"
           else ["plasma-discover", "--application", f"appstream://{args.app_id}"])
before = installed()
with (args.output / "manager.log").open("w") as log:
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 90
        button = None
        names = []
        while time.monotonic() < deadline:
            nodes = list(descendants(Atspi.get_desktop(0)))
            names = [node.get_name() for node in nodes]
            if args.name in names:
                button = next((node for node in nodes if node.get_name() == "Install"
                               and node.get_state_set().contains(Atspi.StateType.SHOWING)
                               and node.get_state_set().contains(Atspi.StateType.SENSITIVE)
                               and node.get_action_iface() is not None), None)
                if before or button is not None:
                    break
            time.sleep(0.3)
        (args.output / "accessible-names.json").write_text(json.dumps(names, indent=2))
        assert args.name in names, "Manager did not expose the requested app details"
        assert installed() == before, "Opening details changed installed state"
        capture("details")
        if args.install:
            assert not before and button is not None, "Fixture must start uninstalled"
            time.sleep(1)  # Let app-details geometry settle after the async load.
            # GTK can replace the accessible button while loading its source.
            # Reacquire it after capture, then use the desktop's real input path.
            button = next(node for node in descendants(Atspi.get_desktop(0))
                          if node.get_name() == "Install"
                          and node.get_state_set().contains(Atspi.StateType.SHOWING)
                          and node.get_state_set().contains(Atspi.StateType.SENSITIVE))
            box = button.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
            assert box.width > 0 and box.height > 0, "Install button has no visible bounds"
            (args.output / "install-button.json").write_text(json.dumps(
                dict(x=box.x, y=box.y, width=box.width, height=box.height)))
            subprocess.run(["xdotool", "mousemove", str(box.x + box.width // 2),
                            str(box.y + box.height // 2), "click", "1"], check=True, timeout=10)
            deadline = time.monotonic() + 300
            retry_at = time.monotonic() + 10
            while not installed() and time.monotonic() < deadline:
                capture("install-progress")
                nodes = list(descendants(Atspi.get_desktop(0)))
                (args.output / "install-progress.json").write_text(json.dumps(
                    [node.get_name() for node in nodes], indent=2))
                # A startup error from an unrelated manager backend can consume
                # the first click. Retry once only while Install is still offered.
                if time.monotonic() >= retry_at:
                    retry_at = float("inf")
                    choices = [node for node in nodes if node.get_name() == "Install"
                               and node.get_state_set().contains(Atspi.StateType.SHOWING)
                               and node.get_state_set().contains(Atspi.StateType.SENSITIVE)]
                    if choices:
                        choices[0].get_action_iface().do_action(0)
                time.sleep(1)
            assert installed(), "Manager did not complete the confirmed installation"
            capture("installed")
        (args.output / "result.json").write_text(json.dumps(
            dict(manager=args.manager, app_id=args.app_id, details=True,
                 installed_before=before, installed_after=installed(),
                 confirmation_clicked=args.install), indent=2) + "\n")
    finally:
        if args.manager == "gnome":
            subprocess.run(["gnome-software", "--quit"], timeout=15, check=False)
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=15)
