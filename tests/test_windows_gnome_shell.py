"""Open windows on GNOME's Wayland session, end to end (ADR 0051): a headless GNOME Shell
with Torchlight's extension installed for the user but not enabled, a private D-Bus bus, a
fixture daemon and the real popup. Checks that the popup enables the extension once, that
the extension lists two test windows most recently used first and switches between them,
and that the popup lists both under their application. Skips without GNOME Shell or the
AT-SPI bindings.

usage: test_windows_gnome_shell.py [--launcher PATH] [--daemon PATH]
Set TORCHLIGHT_GNOME_ROOT to a folder with GNOME Shell unpacked under usr/ (from
`apt-get download` of gnome-shell and its missing dependencies, then `dpkg -x`) to use it
instead of an installed gnome-shell.
"""
import argparse
import glob
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
parser.add_argument("--inside", help=argparse.SUPPRESS)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
UUID = "torchlight@antash-mishra.github.io"
APP_ID = "org.torchlight.TestNotes"
DISPLAY_NAME = "torchlight-gnome"
ACTIVE, INITIALIZED = 1, 6


def shell_root():
    root = os.environ.get("TORCHLIGHT_GNOME_ROOT")
    if root:
        return Path(root) if (Path(root) / "usr/bin/gnome-shell").exists() else None
    return Path("/") if shutil.which("gnome-shell") else None


def wait_for(predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError("condition timed out")


def shell_environment(root, base):
    """GNOME Shell's environment; an unpacked root also needs its libraries, typelibs,
    schemas and data folder."""
    env = dict(os.environ, XDG_CURRENT_DESKTOP="GNOME", XDG_SESSION_TYPE="wayland",
               XDG_SESSION_DESKTOP="gnome", GSETTINGS_BACKEND="keyfile")
    if root == Path("/"):
        return env
    lib = root / "usr/lib/x86_64-linux-gnu"
    schemas = base / "schemas"
    schemas.mkdir()
    for folder in ["/usr/share/glib-2.0/schemas", str(root / "usr/share/glib-2.0/schemas")]:
        for xml in glob.glob(folder + "/*.gschema.xml") + glob.glob(folder + "/*.enums.xml"):
            shutil.copy(xml, schemas)
    subprocess.run(["glib-compile-schemas", str(schemas)], check=True)
    typelibs = [lib / "girepository-1.0", lib / "gjs/girepository-1.0", lib / "mutter-14",
                root / "usr/lib/gnome-shell", root / "usr/lib/gnome-shell/girepository-1.0"]
    env.update(LD_LIBRARY_PATH=f"{lib}:{lib / 'mutter-14'}:{root / 'usr/lib/gnome-shell'}",
               GI_TYPELIB_PATH=":".join(map(str, typelibs)), GSETTINGS_SCHEMA_DIR=str(schemas),
               GNOME_SHELL_DATADIR=str(root / "usr/share/gnome-shell"),
               XDG_DATA_DIRS=f"{root / 'usr/share'}:{env.get('XDG_DATA_DIRS', '/usr/share')}")
    return env


def call(bus, path, interface, method, parameters=None, reply=None):
    from gi.repository import Gio, GLib
    result = bus.call_sync("org.gnome.Shell", path, interface, method, parameters,
                           GLib.VariantType(reply) if reply else None,
                           Gio.DBusCallFlags.NO_AUTO_START, 3000, None)
    return result.unpack() if result is not None else None


def extension_info(bus):
    from gi.repository import GLib
    return call(bus, "/org/gnome/Shell", "org.gnome.Shell.Extensions", "GetExtensionInfo",
                GLib.Variant("(s)", (UUID,)), "(a{sv})")[0]


def listed(bus):
    try:
        return call(bus, "/org/torchlight/Windows", "org.torchlight.Windows", "List", None,
                    "(a(tssss))")[0]
    except Exception:  # The extension is not exported yet.
        return None


def user_dirs(base):
    """The person's folders: the extension (installed, not enabled) and the test app."""
    for name in ["config", "data", "cache", "state", "files"]:
        (base / name).mkdir()
    extension = base / "data/gnome-shell/extensions" / UUID
    extension.mkdir(parents=True)
    for file in ["metadata.json", "extension.js"]:
        shutil.copy(repo / "ui/gnome-shell" / UUID / file, extension)
    applications = base / "data/applications"
    applications.mkdir()
    (applications / f"{APP_ID}.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=Test Notes\nExec=true\nIcon=text-x-generic\n")
    (base / "data/mime").symlink_to("/usr/share/mime", target_is_directory=True)
    (base / "data/icons").symlink_to("/usr/share/icons", target_is_directory=True)
    (base / "config.torchlight").write_text(f"root = {base / 'files'}\n")
    return dict(XDG_CONFIG_HOME=str(base / "config"), XDG_DATA_HOME=str(base / "data"),
                XDG_CACHE_HOME=str(base / "cache"), XDG_STATE_HOME=str(base / "state"))


def accessible_names(Atspi):
    def nodes(node):
        yield node
        for index in range(node.get_child_count()):
            child = node.get_child_at_index(index)
            if child is not None:
                yield from nodes(child)
    frame = next((node for node in nodes(Atspi.get_desktop(0))
                  if node.get_role() == Atspi.Role.FRAME and node.get_name() == "Torchlight"), None)
    return frame, ([node.get_name() for node in nodes(frame)] if frame else [])


def check_popup(base, env, bus, processes):
    """The popup enables the extension once, then lists the windows under their app."""
    import gi
    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi
    assert not extension_info(bus)["enabled"], "the extension starts disabled"
    socket = base / "torchlight.sock"
    # The daemon indexes the fixture's application only, not every one installed here.
    processes.append(subprocess.Popen(
        [str(Path(args.daemon).resolve()), "--config", str(base / "config.torchlight"), "--db",
         str(base / "catalog.db"), "--socket", str(socket)],
        env=dict(env, XDG_DATA_DIRS=str(base / "empty")),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    wait_for(socket.exists)
    processes.append(subprocess.Popen(
        [str(Path(args.launcher).resolve()), "--socket", str(socket), "--toggle"],
        env=dict(env, GDK_BACKEND="wayland"), stdout=open(base / "gtk.log", "w"),
        stderr=subprocess.STDOUT))
    wait_for(lambda: extension_info(bus)["state"] == ACTIVE)
    assert extension_info(bus)["enabled"]
    assert (base / "state/torchlight/gnome-extension-enabled").exists()
    frame, _ = wait_for(lambda: (lambda found: found if found[0] else None)(accessible_names(Atspi)))
    return frame


def type_query(frame, text, Atspi):
    def nodes(node):
        yield node
        for index in range(node.get_child_count()):
            child = node.get_child_at_index(index)
            if child is not None:
                yield from nodes(child)
    editable = next(node for node in nodes(frame) if node.get_editable_text_iface() is not None)
    assert editable.get_editable_text_iface().set_text_contents(text)


def show_again(env, Atspi):
    """The popup reads the open windows each time it shows, so show it again (a toggle hides
    it first when it has focus)."""
    command = [str(Path(args.launcher).resolve()), "--toggle"]
    client = dict(env, GDK_BACKEND="wayland")
    subprocess.run(command, env=client, timeout=10, check=True)
    time.sleep(0.5)
    frame, _ = accessible_names(Atspi)
    if frame is None or not frame.get_state_set().contains(Atspi.StateType.SHOWING):
        subprocess.run(command, env=client, timeout=10, check=True)
    return wait_for(lambda: (lambda found: found if found and found.get_state_set().contains(
        Atspi.StateType.SHOWING) else None)(accessible_names(Atspi)[0]))


def check_windows(bus, env, processes):
    """Two test windows, listed most recently used first, then switched."""
    window = [sys.executable, str(repo / "tests/fixtures/test_window.py"), APP_ID]
    client = dict(env, GDK_BACKEND="wayland")
    processes.append(subprocess.Popen(window + ["First notes"], env=client))
    wait_for(lambda: any(entry[4] == "First notes" for entry in listed(bus) or []))
    processes.append(subprocess.Popen(window + ["Second notes"], env=client))
    windows = wait_for(lambda: (lambda found: found if found and found[0][4] == "Second notes" else None)(
        [entry for entry in listed(bus) or [] if entry[3] == APP_ID]))
    titles = [entry[4] for entry in windows]
    assert titles[:2] == ["Second notes", "First notes"], windows
    first = next(entry[0] for entry in windows if entry[4] == "First notes")
    from gi.repository import GLib
    assert call(bus, "/org/torchlight/Windows", "org.torchlight.Windows", "Activate",
                GLib.Variant("(t)", (first,)), "(b)") == (True,)
    wait_for(lambda: [entry[4] for entry in listed(bus) if entry[3] == APP_ID][0] == "First notes")
    assert call(bus, "/org/torchlight/Windows", "org.torchlight.Windows", "Activate",
                GLib.Variant("(t)", (987654321,)), "(b)") == (False,)


def inside(base):
    import gi
    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi, Gio
    root = shell_root()
    env = dict(shell_environment(root, base), **user_dirs(base))
    processes = []
    log = open(base / "shell.log", "w")
    shell = subprocess.Popen([str(root / "usr/bin/gnome-shell"), "--headless", "--wayland", "--no-x11",
                              "--virtual-monitor", "1920x1080", f"--wayland-display={DISPLAY_NAME}"],
                             env=env, stdout=log, stderr=log)
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        # The shell reads the extension's metadata before it gives it a state.
        info = wait_for(lambda: (lambda value: value if "state" in value else None)(
            _safe(lambda: extension_info(bus), {})), timeout=60)
        assert info["state"] in (INITIALIZED, 2), info
        env = dict(env, WAYLAND_DISPLAY=DISPLAY_NAME)
        env.pop("DISPLAY", None)
        frame = check_popup(base, env, bus, processes)
        check_windows(bus, env, processes)
        frame = show_again(env, Atspi)
        type_query(frame, "notes", Atspi)
        names = wait_for(lambda: (lambda found: found if "First notes" in found and "Second notes" in found
                                  else None)(accessible_names(Atspi)[1]))
        assert "Test Notes" in names, names
    finally:
        for process in reversed(processes):
            process.terminate()
            process.wait(timeout=15)
        shell.terminate()
        shell.wait(timeout=15)
        log.close()
    text = (base / "gtk.log").read_text()
    assert "CRITICAL" not in text and "Gtk-WARNING" not in text, text
    print("GNOME windows passed: the popup enabled the extension once, which listed two windows "
          "most recently used first and switched between them; the popup listed both under their app")


def _safe(function, fallback):
    try:
        return function()
    except Exception:  # GNOME Shell is still starting.
        return fallback


if args.inside:
    inside(Path(args.inside))
    raise SystemExit(0)
if shell_root() is None:
    print("GNOME windows check skipped: gnome-shell is not installed")
    raise SystemExit(0)
try:
    import gi
    gi.require_version("Atspi", "2.0")
    gi.require_version("Gtk", "4.0")
except (ImportError, ValueError):
    print("GNOME windows check skipped: the AT-SPI or GTK 4 Python bindings are not installed")
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="torchlight-gnome-") as temporary:
    base = Path(temporary)
    runtime = base / "runtime"
    runtime.mkdir(mode=0o700)
    services = base / "services"
    services.mkdir()
    (services / "org.a11y.Bus.service").symlink_to("/usr/share/dbus-1/services/org.a11y.Bus.service")
    (base / "bus.conf").write_text(f"""<busconfig><type>session</type><listen>unix:tmpdir={runtime}</listen>
<auth>EXTERNAL</auth><servicedir>{services}</servicedir><policy context="default">
<allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>""")
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime))
    for key in ["WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS"]:
        env.pop(key, None)
    result = subprocess.run(["dbus-run-session", "--config-file=" + str(base / "bus.conf"), "--",
                             sys.executable, __file__, "--launcher", args.launcher, "--daemon",
                             args.daemon, "--inside", str(base)], env=env)
    if result.returncode:
        for name in ["gtk.log", "shell.log"]:
            if (base / name).exists():
                print(f"--- {name}\n" + (base / name).read_text()[-4000:])
    raise SystemExit(result.returncode)
