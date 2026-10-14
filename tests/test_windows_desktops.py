"""Open windows on Wayland desktops other than GNOME, end to end (ADR 0052 to 0054): Sway and
labwc (headless), COSMIC and Cinnamon (nested on a private X display) and KWin (virtual), each
on a private D-Bus bus with a fixture daemon, two test windows and the real popup. For each
desktop it checks that the popup lists both windows under their application, most recently
used first; that the session's window source (tests/fixtures/window_sources.c) switches to
the older window; and that the popup then lists that window first. On Cinnamon the popup also
turns its extension on once, without a new login. Desktops that are not available are
skipped, and everything is skipped without the AT-SPI bindings.

usage: test_windows_desktops.py [--launcher PATH] [--daemon PATH] [--sources PATH]
                                [--desktop NAME ...]
Desktops come from the system, or from folders where their packages were unpacked
(`apt-get download`, then `dpkg -x`):
  TORCHLIGHT_WLROOTS_ROOT  sway and labwc, with libwlroots12t64 and their missing libraries
  TORCHLIGHT_COSMIC_ROOT   cosmic-comp from Pop!_OS's archive, with libdisplay-info1, libseat1
  TORCHLIGHT_KWIN_ROOT     kwin-wayland 5.27 (Ubuntu 24.04) or 6 (KDE neon), with libraries
Cinnamon is used when it is installed. Nested desktops need Xvfb.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

DESKTOPS = ["sway", "labwc", "cosmic", "kwin", "cinnamon"]
parser = argparse.ArgumentParser()
parser.add_argument("--launcher", default="build/torchlight-gtk")
parser.add_argument("--daemon", default="build/torchlightd")
parser.add_argument("--sources", default="build/test_window_sources")
parser.add_argument("--desktop", action="append", choices=DESKTOPS)
parser.add_argument("--inside", nargs=2, help=argparse.SUPPRESS)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
APP_ID = "org.torchlight.TestNotes"
UUID = "torchlight@antash-mishra.github.io"
# Opened in this order; each opened window takes the focus.
TITLES = ["First notes", "Second notes"]
LIB = "usr/lib/x86_64-linux-gnu"
SCREEN = ("1280", "800")


def program(binary, root_variable):
    """(executable, root) for binary from the root in root_variable, else the system's."""
    root = os.environ.get(root_variable) if root_variable else None
    if root:
        path = Path(root) / "usr/bin" / binary
        return (path, Path(root)) if path.exists() else None
    found = shutil.which(binary)
    return (Path(found), Path("/")) if found else None


def available(desktop):
    binary, variable = {"sway": ("sway", "TORCHLIGHT_WLROOTS_ROOT"),
                        "labwc": ("labwc", "TORCHLIGHT_WLROOTS_ROOT"),
                        "cosmic": ("cosmic-comp", "TORCHLIGHT_COSMIC_ROOT"),
                        "kwin": ("kwin_wayland", "TORCHLIGHT_KWIN_ROOT"),
                        "cinnamon": ("cinnamon", None)}[desktop]
    found = program(binary, variable)
    nested = desktop in ("cosmic", "cinnamon")
    return found if found and (not nested or shutil.which("Xvfb")) else None


def wait_for(predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError("condition timed out")


def libraries(root, *extra):
    if root == Path("/"):
        return {}
    return {"LD_LIBRARY_PATH": ":".join([str(root / LIB), *map(str, extra)])}


def nested_display(processes, base):
    """A private X display for desktops that run as a window; its name."""
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", "1", "-screen", "0", "x".join(SCREEN) + "x24",
                             "-nolisten", "tcp"], stdout=subprocess.PIPE,
                            stderr=open(base / "xvfb.log", "w"))
    processes.append(xvfb)
    return ":" + xvfb.stdout.readline().decode().strip()


def start_desktop(desktop, found, base, env, processes):
    """Start the desktop; the environment its clients need."""
    binary, root = found
    log = open(base / "desktop.log", "w")
    wlroots = dict(env, WLR_BACKENDS="headless", WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1",
                   **libraries(root))
    client = {"XDG_CURRENT_DESKTOP": {"sway": "sway", "labwc": "labwc:wlroots", "cosmic": "COSMIC",
                                      "kwin": "KDE", "cinnamon": "X-Cinnamon"}[desktop]}
    if desktop == "sway":
        (base / "sway.conf").write_text("xwayland disable\noutput HEADLESS-1 resolution 1280x800\n")
        # Sway refuses to start beside NVIDIA's driver unless told; it renders in software here.
        command, desktop_env = [binary, "--unsupported-gpu", "-c", base / "sway.conf"], wlroots
    elif desktop == "labwc":
        (base / "labwc").mkdir()
        (base / "labwc/rc.xml").write_text("<?xml version='1.0'?><labwc_config/>\n")
        command, desktop_env = [binary, "-C", base / "labwc"], wlroots
    elif desktop == "cosmic":
        # Window state only arrives as COSMIC renders, which needs Mesa's software renderer.
        command = [binary]
        desktop_env = dict(env, DISPLAY=nested_display(processes, base), LIBGL_ALWAYS_SOFTWARE="1",
                           **libraries(root))
    elif desktop == "kwin":
        qt = "qt6" if (root / LIB / "qt6").exists() else "qt5"
        plugins = [root / LIB / qt / "plugins"] + ([Path("/") / LIB / qt / "plugins"] if qt == "qt5" else [])
        command = [binary, "--virtual", "--no-lockscreen", "--no-global-shortcuts",
                   "--socket", "torchlight-kwin", "--width", SCREEN[0], "--height", SCREEN[1]]
        desktop_env = dict(env, KWIN_COMPOSE="Q", QT_PLUGIN_PATH=":".join(map(str, plugins)),
                           QML2_IMPORT_PATH=str(root / LIB / qt / "qml"),
                           QML_IMPORT_PATH=str(root / LIB / qt / "qml"),
                           XDG_DATA_DIRS=f"{root / 'usr/share'}:/usr/share", **libraries(root))
    else:
        command = [binary, "--wayland", "--nested", "--sm-disable"]
        desktop_env = dict(env, DISPLAY=nested_display(processes, base))
    processes.append(subprocess.Popen([str(part) for part in command], env=desktop_env,
                                      stdout=log, stderr=log))
    runtime = Path(env["XDG_RUNTIME_DIR"])
    socket = wait_for(lambda: next((path.name for path in runtime.glob("*")
                                    if path.is_socket() and (path.name.startswith("wayland-")
                                                             or path.name == "torchlight-kwin")), None))
    client.update(WAYLAND_DISPLAY=socket, GDK_BACKEND="wayland", GSK_RENDERER="cairo")
    return client


def user_dirs(base, desktop):
    """The person's folders: the test application, and Cinnamon's extension for Cinnamon."""
    for name in ["config", "data", "cache", "state", "files", "empty"]:
        (base / name).mkdir()
    applications = base / "data/applications"
    applications.mkdir()
    (applications / f"{APP_ID}.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=Test Notes\nExec=true\nIcon=text-x-generic\n")
    (base / "data/mime").symlink_to("/usr/share/mime", target_is_directory=True)
    (base / "data/icons").symlink_to("/usr/share/icons", target_is_directory=True)
    if desktop == "cinnamon":
        shutil.copytree(repo / "ui/cinnamon" / UUID, base / "data/cinnamon/extensions" / UUID)
    (base / "daemon.conf").write_text(f"root = {base / 'files'}\n")
    return dict(XDG_CONFIG_HOME=str(base / "config"), XDG_DATA_HOME=str(base / "data"),
                XDG_CACHE_HOME=str(base / "cache"), XDG_STATE_HOME=str(base / "state"),
                HOME=str(base))


def accessible_nodes(node):
    yield node
    for index in range(node.get_child_count()):
        child = node.get_child_at_index(index)
        if child is not None:
            yield from accessible_nodes(child)


def frame_named(Atspi, name):
    try:
        return next((node for node in accessible_nodes(Atspi.get_desktop(0))
                     if node.get_role() == Atspi.Role.FRAME and node.get_name() == name), None)
    except Exception:  # A window is being built or torn down; ask again.
        return None


def sources(base, env, title=None):
    """The session's window source's list (TITLES order) and, with title, its list after
    switching to that window."""
    command = [str(Path(args.sources).resolve()), str(base / "sources-state")] + ([title] if title else [])
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    parts = result.stdout.split("--\n")
    lists = [[line.split("\t")[4] for line in part.splitlines() if line.split("\t")[1] == APP_ID]
             for part in parts]
    return [[name for name in names if name in TITLES] for names in lists]


def showing(Atspi):
    frame = frame_named(Atspi, "Torchlight")
    return frame if frame is not None and frame.get_state_set().contains(Atspi.StateType.SHOWING) else None


def popup_order(Atspi, env):
    """Show the popup again (it reads the open windows each time it shows), search for the
    notes and return their windows in the order listed."""
    command = [str(Path(args.launcher).resolve()), "--toggle"]
    subprocess.run(command, env=env, timeout=10, check=True)
    time.sleep(0.5)
    if showing(Atspi) is None:
        # It had focus, so the toggle hid it; the next one shows it.
        subprocess.run(command, env=env, timeout=10, check=True)
    frame = wait_for(lambda: showing(Atspi))
    editable = wait_for(lambda: next((node for node in accessible_nodes(frame)
                                      if node.get_editable_text_iface() is not None), None))
    assert editable.get_editable_text_iface().set_text_contents("notes")

    def listed():
        try:
            names = [node.get_name() for node in accessible_nodes(frame_named(Atspi, "Torchlight"))]
        except Exception:  # The list is being rebuilt.
            return None
        if "Test Notes" not in names or any(title not in names for title in TITLES):
            return None
        return sorted(TITLES, key=names.index)
    return wait_for(listed)


def cinnamon_extension(base):
    """Whether the popup listed the extension in Cinnamon's settings and wrote its marker."""
    keyfile = base / "config/glib-2.0/settings/keyfile"
    text = keyfile.read_text() if keyfile.exists() else ""
    return UUID in text and (base / "state/torchlight/cinnamon-extension-enabled").exists()


def run(desktop, found, base):
    import gi
    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi
    processes = []
    env = dict(os.environ, **user_dirs(base, desktop), GSETTINGS_BACKEND="keyfile")
    for key in ["DISPLAY", "WAYLAND_DISPLAY"]:
        env.pop(key, None)
    try:
        client = dict(env, **start_desktop(desktop, found, base, env, processes))
        socket = base / "torchlight.sock"
        # The daemon indexes the fixture's application only, not every one installed here.
        processes.append(subprocess.Popen(
            [str(Path(args.daemon).resolve()), "--config", str(base / "daemon.conf"), "--db",
             str(base / "catalog.db"), "--socket", str(socket)],
            env=dict(env, XDG_DATA_DIRS=str(base / "empty")),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        wait_for(socket.exists)
        for title in TITLES:
            processes.append(subprocess.Popen(
                [sys.executable, str(repo / "tests/fixtures/test_window.py"), APP_ID, title],
                env=client, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            wait_for(lambda: frame_named(Atspi, title))
            time.sleep(0.5)
        processes.append(subprocess.Popen(
            [str(Path(args.launcher).resolve()), "--socket", str(socket), "--toggle"],
            env=client, stdout=open(base / "gtk.log", "w"), stderr=subprocess.STDOUT))
        wait_for(lambda: frame_named(Atspi, "Torchlight"))
        if desktop == "cinnamon":
            # The popup turned the extension on; Cinnamon loads it at once.
            wait_for(lambda: cinnamon_extension(base))
        # The source lists both windows. A source just started cannot know which was used
        # last while another window has focus, so only the popup's order is checked.
        wait_for(lambda: sorted(sources(base, client)[0]) == sorted(TITLES))
        # The window opened last was used last.
        newest_first = TITLES[::-1]
        shown = popup_order(Atspi, client)
        assert shown == newest_first, shown
        # The popup may still be showing; the source switches windows all the same.
        older = newest_first[1]
        before, after = sources(base, client, older)
        assert sorted(before) == sorted(TITLES) and after == TITLES, (before, after)
        shown = wait_for(lambda: (lambda order: order if order == TITLES else None)(
            popup_order(Atspi, client)))
        print(f"{desktop}: the popup listed {newest_first}; switching to {older!r} made it the "
              f"most recent, and the popup then listed {shown}")
    finally:
        for process in reversed(processes):
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    text = (base / "gtk.log").read_text()
    assert "CRITICAL" not in text and "Gtk-WARNING" not in text, text


if args.inside:
    desktop = args.inside[0]
    run(desktop, available(desktop), Path(args.inside[1]))
    raise SystemExit(0)
try:
    import gi
    gi.require_version("Atspi", "2.0")
    gi.require_version("Gtk", "4.0")
except (ImportError, ValueError):
    print("Desktop windows check skipped: the AT-SPI or GTK 4 Python bindings are not installed")
    raise SystemExit(0)
failed = []
for desktop in args.desktop or DESKTOPS:
    if available(desktop) is None:
        print(f"{desktop}: skipped, not installed (see this file's header)")
        continue
    with tempfile.TemporaryDirectory(prefix=f"torchlight-{desktop}-") as temporary:
        base = Path(temporary)
        runtime = base / "runtime"
        runtime.mkdir(mode=0o700)
        services = base / "services"
        services.mkdir()
        (services / "org.a11y.Bus.service").symlink_to("/usr/share/dbus-1/services/org.a11y.Bus.service")
        # Only the accessibility bus can be started on demand: no portals, no session services.
        (base / "bus.conf").write_text(f"""<busconfig><type>session</type><listen>unix:tmpdir={runtime}</listen>
<auth>EXTERNAL</auth><servicedir>{services}</servicedir><policy context="default">
<allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>""")
        (base / "run").mkdir()
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime))
        for key in ["WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS"]:
            env.pop(key, None)
        result = subprocess.run(["dbus-run-session", "--config-file=" + str(base / "bus.conf"), "--",
                                 sys.executable, __file__, "--launcher", args.launcher, "--daemon",
                                 args.daemon, "--sources", args.sources, "--inside", desktop,
                                 str(base / "run")], env=env)
        if result.returncode:
            failed.append(desktop)
            for name in ["gtk.log", "desktop.log"]:
                if (base / "run" / name).exists():
                    print(f"--- {desktop} {name}\n" + (base / "run" / name).read_text()[-3000:])
raise SystemExit(1 if failed else 0)
