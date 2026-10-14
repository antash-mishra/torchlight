"""Popup acceptance on a native Wayland session: a headless Weston compositor, a private
D-Bus bus and a fixture daemon. Wayland has no xdotool, so the query is typed through
AT-SPI. Checks that the popup opens, sizes itself to the monitor (wide enough for the
preview panel), searches, groups results and describes the selection, without GTK
warnings. Skips when Weston or the AT-SPI bindings are missing.

usage: test_popup_wayland.py [--launcher PATH] [--daemon PATH]
Set TORCHLIGHT_WESTON_ROOT to a directory with Weston unpacked under usr/ (for example
from `apt-get download weston libweston-13-0` and `dpkg -x`) to use it instead of an
installed weston.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--launcher", default="build/torchlight-gtk")
parser.add_argument("--daemon", default="build/torchlightd")
parser.add_argument("--inside", nargs=2, help=argparse.SUPPRESS)
args = parser.parse_args()
# The popup asks for 880 logical pixels when the monitor has room for its preview panel.
PREVIEW_WIDTH = 880
SCREEN = (1920, 1080)
# Well above the fitted 377px window for two results, well below the 712px regression.
COMPACT_HEIGHT = 520


def weston_command():
    """Return (argv prefix, environment additions) for Weston, or None when absent."""
    root = os.environ.get("TORCHLIGHT_WESTON_ROOT")
    if root:
        lib = Path(root) / "usr/lib/x86_64-linux-gnu"
        modules = {"headless-backend.so": lib / "libweston-13/headless-backend.so",
                   "desktop-shell.so": lib / "weston/desktop-shell.so"}
        extra = {"WESTON_MODULE_MAP": ";".join(f"{k}={v}" for k, v in modules.items()),
                 "LD_LIBRARY_PATH": f"{lib}:{lib / 'weston'}"}
        return [str(Path(root) / "usr/bin/weston")], extra, Path(root) / "usr/libexec/weston-desktop-shell"
    weston = shutil.which("weston")
    if weston is None:
        return None
    return [weston], {}, None


def accessible_nodes(node):
    yield node
    for index in range(node.get_child_count()):
        child = node.get_child_at_index(index)
        if child is not None:
            yield from accessible_nodes(child)


def wait_for(predicate, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError("condition timed out")


def fixture(base):
    """A file and a settings panel to find, and a daemon config indexing them."""
    files = base / "files"
    files.mkdir()
    (files / "sound-notes.md").write_text("# Sound\n")
    apps = base / "data/applications"
    apps.mkdir(parents=True)
    (apps.parent / "mime").symlink_to("/usr/share/mime", target_is_directory=True)
    (apps.parent / "icons").symlink_to("/usr/share/icons", target_is_directory=True)
    (apps / "sound.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=Sound\nComment=Change sound levels and devices\n"
        "Exec=true\nCategories=Settings;\nKeywords=audio;volume;\nIcon=preferences-desktop-sound\n")
    config = base / "config"
    config.write_text(f"root = {files}\n")
    return apps.parent, config


def check_popup(frame, Atspi):
    editable = next(node for node in accessible_nodes(frame) if node.get_editable_text_iface())
    assert editable.get_editable_text_iface().set_text_contents("sound")
    names = wait_for(lambda: (lambda found: found if "Settings panel" in found else None)(
        [node.get_name() for node in accessible_nodes(frame)]))
    assert "Sound" in names and "sound-notes.md" in names, names
    assert "Change sound levels and devices" in names, names
    extents = frame.get_component_iface().get_extents(Atspi.CoordType.WINDOW)
    # The window adds at most a 20px shadow margin on each side of the 880px surface.
    assert PREVIEW_WIDTH <= extents.width <= PREVIEW_WIDTH + 40, extents.width
    # Two results and a short preview: the window fits them. Before the preview's labels
    # had a natural width, GTK's Wayland sizing made this window 712px tall.
    assert extents.height < COMPACT_HEIGHT, extents.height
    return extents


def inside(base, out):
    """Runs inside the private bus: compositor, daemon, popup and the checks."""
    import gi
    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi
    weston, extra, shell_client = weston_command()
    env = dict(os.environ, **extra)
    ini = base / "weston.ini"
    shell = f"client={shell_client}\n" if shell_client else ""
    ini.write_text(f"[core]\nidle-time=0\n[shell]\n{shell}panel-position=none\n")
    log = open(out / "weston.log", "w")
    compositor = subprocess.Popen([*weston, "--backend=headless", "--renderer=pixman",
                                   f"--width={SCREEN[0]}", f"--height={SCREEN[1]}",
                                   "--socket=torchlight-wl", f"--config={ini}", "--shell=desktop"],
                                  env=env, stdout=log, stderr=log)
    data, config = fixture(base)
    socket = base / "torchlight.sock"
    # Only the fixture's entries; mime and icons are linked into its data directory.
    common = dict(os.environ, XDG_DATA_HOME=str(data), XDG_DATA_DIRS=str(base / "empty"),
                  XDG_CURRENT_DESKTOP="X-Cinnamon")
    daemon = subprocess.Popen([str(Path(args.daemon).resolve()), "--config", str(config), "--db",
                               str(base / "catalog.db"), "--socket", str(socket)], env=common,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    gui_log = open(out / "gtk.log", "w")
    gui = None
    try:
        wait_for(lambda: (Path(os.environ["XDG_RUNTIME_DIR"]) / "torchlight-wl").exists())
        wait_for(socket.exists)
        client = dict(common, WAYLAND_DISPLAY="torchlight-wl", GDK_BACKEND="wayland")
        client.pop("DISPLAY", None)
        gui = subprocess.Popen([str(Path(args.launcher).resolve()), "--socket", str(socket), "--toggle"],
                               env=client, stdout=gui_log, stderr=gui_log)
        frame = wait_for(lambda: next((node for node in accessible_nodes(Atspi.get_desktop(0))
                                       if node.get_role() == Atspi.Role.FRAME
                                       and node.get_name() == "Torchlight"), None))
        extents = check_popup(frame, Atspi)
    finally:
        if gui is not None:
            gui.terminate()
            gui.wait(timeout=10)
        daemon.terminate()
        daemon.wait(timeout=15)
        compositor.terminate()
        compositor.wait(timeout=10)
    text = (out / "gtk.log").read_text()
    assert "CRITICAL" not in text and "Gtk-WARNING" not in text and "ERROR" not in text, text
    print(f"Wayland popup passed: {extents.width}x{extents.height}, preview panel, grouped results")


if args.inside:
    inside(Path(args.inside[0]), Path(args.inside[1]))
    raise SystemExit(0)
if weston_command() is None:
    print("Wayland popup check skipped: weston is not installed")
    raise SystemExit(0)
try:
    import gi
    gi.require_version("Atspi", "2.0")
except (ImportError, ValueError):
    print("Wayland popup check skipped: the AT-SPI Python bindings are not installed")
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="torchlight-wayland-") as temporary:
    base = Path(temporary)
    runtime = base / "runtime"
    runtime.mkdir(mode=0o700)
    services = base / "services"
    services.mkdir()
    (services / "org.a11y.Bus.service").symlink_to("/usr/share/dbus-1/services/org.a11y.Bus.service")
    bus = base / "bus.conf"
    bus.write_text(f"""<busconfig><type>session</type><listen>unix:tmpdir={runtime}</listen>
<auth>EXTERNAL</auth><servicedir>{services}</servicedir><policy context="default">
<allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>""")
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime))
    env.pop("WAYLAND_DISPLAY", None)
    command = [sys.executable, __file__, "--launcher", args.launcher, "--daemon", args.daemon,
               "--inside", str(base), str(base)]
    result = subprocess.run(["dbus-run-session", "--config-file=" + str(bus), "--", *command], env=env)
    if result.returncode:
        for name in ["gtk.log", "weston.log"]:
            if (base / name).exists():
                print(f"--- {name}\n" + (base / name).read_text()[-3000:])
    raise SystemExit(result.returncode)
