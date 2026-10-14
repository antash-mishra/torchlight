#!/usr/bin/env python3
"""Record the README's demo of the popup (docs/ui/demo.gif) on a private X display.

Everything in it is made up: a home folder at /home/demo with notes and projects, a handful
of applications and settings panels, and two Terminal windows. Nothing from this machine's
own files, applications or windows appears. The script opens the popup and types a few
searches the way a person would: an application with its open windows, a typo, a folder
name and a settings panel.

It runs in a user and mount namespace (no root needed), where /home/demo is the fixture,
and records with GTK's CPU renderer so that a loaded machine still keeps up with the typing;
that renderer draws no shadow.

Needs `make` (the daemon and popup), unshare, Xvfb, metacity, xdotool, xsetroot, ffmpeg, the
GTK 4 Python bindings (for the fixture windows) and the popup's fonts installed.

usage: scripts/record_demo.py [--output docs/ui/demo.gif] [--fps 15] [--raw FILE.mkv]
"""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--output", default="docs/ui/demo.gif")
parser.add_argument("--fps", type=int, default=15)
parser.add_argument("--raw", help="also keep the uncropped, lossless recording here")
parser.add_argument("--namespace", help=argparse.SUPPRESS)
parser.add_argument("--display", help=argparse.SUPPRESS)
parser.add_argument("--inside", help=argparse.SUPPRESS)
parser.add_argument("--ids", help=argparse.SUPPRESS)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
HOME = Path("/home/demo")
SCREEN = (1280, 800)
# GitHub's dark page color, so the recording blends into the README there. Without a
# compositor the popup's rounded corners are black, which hardly shows against it.
BACKGROUND = "#0d1117"
# Room kept around the popup's largest size.
CROP_MARGIN = 24
# The GIF starts this long after the first search is typed, on its results: a viewer with
# animation off sees only the first frame. The loop plays on unchanged from there.
POSTER_SETTLE = 1.2
TYPE_DELAY_MS = 110
DAY = 86400
# Made-up files: path, size in bytes, age in days.
FILES = [
    ("Documents/Work/notes/meeting-notes.md", 1840, 0.2),
    ("Documents/Work/notes/roadmap.md", 2310, 3),
    ("Documents/Work/budget-2026.ods", 18432, 12),
    ("Documents/Personal/notes/recipes.md", 3020, 40),
    ("Projects/torchlight/README.md", 4180, 1),
    ("Projects/torchlight/src/main.c", 12760, 1),
    ("Projects/torchlight/docs/terminal-notes.md", 2050, 6),
    ("Projects/website/README.md", 1560, 30),
    ("Downloads/terminal-themes.zip", 245760, 9),
    ("Pictures/terminals/prompt.png", 98304, 21),
]
APPS = {
    "org.gnome.Terminal": ("Terminal", "Use the command line", "utilities-terminal", "System;"),
    "org.gnome.TextEditor": ("Text Editor", "Edit text files", "accessories-text-editor",
                             "Utility;TextEditor;"),
    "nemo": ("Files", "Access and organize files", "system-file-manager", "System;FileManager;"),
    "display": ("Display", "Change resolution and position of monitors",
                "preferences-desktop-display", "Settings;HardwareSettings;"),
    "sound": ("Sound", "Change sound levels and devices", "preferences-desktop-sound",
              "Settings;HardwareSettings;"),
}
# The two Terminal windows the popup lists under Terminal.
WINDOWS = ["~/Projects/torchlight", "~/Documents/Work"]
# What the recording types, and how long it rests on each result.
SCENES = [
    ("type", "term", 2.6),
    ("key", "Down", 0.6),
    ("key", "Down", 1.6),
    ("replace", "raedme", 2.4),
    ("replace", "work notes", 2.6),
    ("replace", "disp", 2.2),
]


def wait_for(predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    raise SystemExit("record_demo: timed out waiting for the desktop")


def text_of(size):
    line = "Notes, plans and a few links to keep.\n"
    return (line * (size // len(line) + 1))[:size]


def fixture(base):
    """The made-up home folder, its applications and fonts, and the daemon's configuration."""
    home = base / "home"
    now = time.time()
    for path, size, age in FILES:
        target = home / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.suffix in (".md", ".c"):
            target.write_text(text_of(size))
        else:
            target.write_bytes(bytes(size))
        os.utime(target, (now - age * DAY, now - age * DAY))
    data = home / ".local/share"
    applications = data / "applications"
    applications.mkdir(parents=True)
    for desktop_id, (name, comment, icon, categories) in APPS.items():
        (applications / f"{desktop_id}.desktop").write_text(
            f"[Desktop Entry]\nType=Application\nName={name}\nComment={comment}\nExec=true\n"
            f"Icon={icon}\nCategories={categories}\n")
    (data / "mime").symlink_to("/usr/share/mime", target_is_directory=True)
    (data / "icons").symlink_to("/usr/share/icons", target_is_directory=True)
    # The popup's typefaces, from wherever they are installed for the real user.
    fonts = data / "fonts"
    fonts.mkdir()
    for folder in [Path.home() / ".local/share/fonts/torchlight",
                   Path("/usr/share/fonts/truetype/torchlight")]:
        for font in folder.glob("*") if folder.is_dir() else []:
            shutil.copy(font, fonts)
    settings = home / ".config/gtk-4.0"
    settings.mkdir(parents=True)
    (settings / "settings.ini").write_text(
        "[Settings]\ngtk-font-name=Noto Mono 13\ngtk-icon-theme-name=Mint-Y\n")
    (base / "daemon.conf").write_text(f"root = {HOME}\n")


def mount(source, target, kind=None):
    """mount(2) itself, as the namespace's root (mount(8) refuses other users). A bind mount
    without a kind."""
    libc = ctypes.CDLL(None, use_errno=True)
    flags = 0 if kind else 4096  # MS_BIND
    if libc.mount(str(source).encode(), str(target).encode(),
                  kind.encode() if kind else None, flags, None) != 0:
        raise SystemExit(f"record_demo: mounting {target}: {os.strerror(ctypes.get_errno())}")


def mount_home(base):
    """In the namespace: the repository stays reachable under base, and /home holds only the
    fixture, as /home/demo."""
    root = base / "repo"
    root.mkdir()
    mount(repo, root)
    mount("tmpfs", "/home", "tmpfs")
    HOME.mkdir()
    mount(base / "home", HOME)
    return root


def xdo(env, *arguments):
    return subprocess.run(["xdotool", *map(str, arguments)], env=env, capture_output=True,
                          text=True).stdout.strip()


def popup_geometry(env):
    window = wait_for(lambda: xdo(env, "search", "--onlyvisible", "--name", "^Torchlight$"))
    shell = xdo(env, "getwindowgeometry", "--shell", window.split()[-1])
    return {key: int(value) for key, value in (line.split("=", 1) for line in shell.splitlines())
            if key in ("X", "Y", "WIDTH", "HEIGHT")}


def play(env, toggle, started):
    """Open the popup and run the scenes; return the popup's largest extent and the poster's
    time, in seconds since started (when the recording began, on time.monotonic())."""
    subprocess.run(toggle, env=env, check=True)
    time.sleep(1.4)
    largest = popup_geometry(env)
    poster = None
    for kind, text, rest in SCENES:
        if kind == "replace":
            xdo(env, "key", "ctrl+a")
            time.sleep(0.25)
        if kind in ("type", "replace"):
            xdo(env, "type", "--delay", TYPE_DELAY_MS, text)
        else:
            xdo(env, "key", text)
        if poster is None:
            poster = time.monotonic() - started + POSTER_SETTLE
        time.sleep(rest)
        geometry = popup_geometry(env)
        largest["HEIGHT"] = max(largest["HEIGHT"], geometry["HEIGHT"])
    xdo(env, "key", "Escape")
    time.sleep(1.0)
    return largest, poster


def start_desktop(env, root, base, processes):
    """The desktop: a plain background, a window manager, two Terminal windows (minimized,
    still listed) and the daemon over the fixture."""
    # No compositor, so the root window's color shows. Metacity keeps that choice in its
    # settings, which the private bus cannot store (no dconf): it would fall back to
    # compositing and paint the background black. In memory it keeps it.
    processes.append(subprocess.Popen(["metacity", "--sm-disable", "--compositor=none"],
                                      env=dict(env, GSETTINGS_BACKEND="memory"),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    time.sleep(0.5)
    # Once the window manager is connected: Xvfb resets, losing the color, whenever its last
    # client leaves.
    subprocess.run(["xsetroot", "-solid", BACKGROUND], env=env, check=True)
    # The pointer rests in a corner, away from the popup and its tooltips.
    xdo(env, "mousemove", SCREEN[0] - 1, SCREEN[1] - 1)
    for title in WINDOWS:
        processes.append(subprocess.Popen(
            [sys.executable, str(root / "tests/fixtures/test_window.py"), "org.gnome.Terminal",
             title], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        window = wait_for(lambda: xdo(env, "search", "--onlyvisible", "--name", f"^{title}$"))
        xdo(env, "windowminimize", window.split()[-1])
    socket = base / "torchlight.sock"
    processes.append(subprocess.Popen(
        [str(root / "build/torchlightd"), "--config", str(base / "daemon.conf"), "--db",
         str(base / "catalog.db"), "--socket", str(socket)],
        # Only the fixture's applications, not the ones installed on this machine.
        env=dict(env, XDG_DATA_DIRS=str(base / "empty")),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    wait_for(socket.exists)
    time.sleep(2.0)  # the first index
    return socket


def record(base, root):
    """Inside the private bus and display: the desktop, the popup and the recording."""
    env = dict(os.environ, HOME=str(HOME), XDG_CONFIG_HOME=str(HOME / ".config"),
               XDG_DATA_HOME=str(HOME / ".local/share"), XDG_CACHE_HOME=str(HOME / ".cache"),
               XDG_STATE_HOME=str(HOME / ".local/state"), GTK_THEME="Mint-Y-Dark",
               GSK_RENDERER="cairo", GDK_BACKEND="x11", XDG_CURRENT_DESKTOP="X-Cinnamon")
    processes = []
    try:
        socket = start_desktop(env, root, base, processes)
        toggle = [str(root / "build/torchlight-gtk"), "--socket", str(socket), "--toggle"]
        # Start the resident popup once and hide it, so the recording shows a warm opening.
        processes.append(subprocess.Popen(toggle, env=env, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL))
        popup_geometry(env)
        time.sleep(1.0)
        xdo(env, "key", "Escape")
        time.sleep(0.8)
        ffmpeg = subprocess.Popen(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab", "-draw_mouse", "0",
             "-framerate", str(args.fps), "-video_size", f"{SCREEN[0]}x{SCREEN[1]}", "-i",
             env["DISPLAY"], "-c:v", "libx264rgb", "-crf", "0", "-preset", "ultrafast",
             str(base / "raw.mkv")], env=env, stdin=subprocess.PIPE)
        started = time.monotonic()
        time.sleep(0.8)
        largest, poster = play(env, toggle, started)
        ffmpeg.communicate(b"q", timeout=30)
        (base / "crop").write_text(" ".join(str(largest[key]) for key in ("X", "Y", "WIDTH",
                                                                           "HEIGHT")))
        (base / "poster").write_text(f"{poster:.2f}")
    finally:
        for process in reversed(processes):
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()


def run_display(base, root):
    """In the namespace: a private display and session bus around record()."""
    runtime = base / "runtime"
    runtime.mkdir(mode=0o700)
    # A private bus with no services: no portals or keyrings for GTK to wait on.
    (base / "bus.conf").write_text(f"""<busconfig><type>session</type>
<listen>unix:tmpdir={runtime}</listen><auth>EXTERNAL</auth><policy context="default">
<allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>""")
    server = subprocess.Popen(["Xvfb", "-displayfd", "1", "-screen", "0",
                               f"{SCREEN[0]}x{SCREEN[1]}x24", "-nolisten", "tcp"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    try:
        env = dict(os.environ, DISPLAY=":" + server.stdout.readline().strip(),
                   XDG_RUNTIME_DIR=str(runtime), GTK_A11Y="none")
        for key in ["WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"]:
            env.pop(key, None)
        subprocess.run(["dbus-run-session", f"--config-file={base / 'bus.conf'}", "--",
                        sys.executable, str(root / "scripts/record_demo.py"), "--inside",
                        str(base), "--fps", str(args.fps)], env=env, check=True)
    finally:
        server.terminate()
        server.wait(timeout=10)


def convert(base, output):
    """Crop to the popup and its surroundings, start the loop at the poster, then make a GIF
    with a palette per scene."""
    x, y, width, height = map(int, (base / "crop").read_text().split())
    poster = (base / "poster").read_text()
    left, top = max(0, x - CROP_MARGIN), max(0, y - CROP_MARGIN)
    width = min(SCREEN[0] - left, width + 2 * CROP_MARGIN) // 2 * 2
    height = min(SCREEN[1] - top, height + 2 * CROP_MARGIN) // 2 * 2
    filters = (f"crop={width}:{height}:{left}:{top},split[a][b];"
               f"[a]trim=start={poster},setpts=PTS-STARTPTS[late];"
               f"[b]trim=end={poster},setpts=PTS-STARTPTS[early];"
               "[late][early]concat,split[c][d];[c]palettegen=stats_mode=diff[p];"
               "[d][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle")
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(base / "raw.mkv"),
                    "-filter_complex", filters, "-loop", "0", str(output)], check=True)
    print(f"{output}: {width}x{height}, {output.stat().st_size // 1024} KiB")


if args.inside:
    record(Path(args.inside), Path(args.inside) / "repo")
elif args.display:
    run_display(Path(args.display), Path(args.display) / "repo")
elif args.namespace:
    # Mounting needs the namespace's root; the desktop then runs as the person again, in a
    # nested namespace that keeps these mounts.
    base = Path(args.namespace)
    root = mount_home(base)
    uid, gid = args.ids.split(":")
    subprocess.run(["unshare", "--user", f"--map-user={uid}", f"--map-group={gid}", "--",
                    sys.executable, str(root / "scripts/record_demo.py"), "--display", str(base),
                    "--fps", str(args.fps)], check=True)
else:
    missing = [tool for tool in ["unshare", "Xvfb", "metacity", "xdotool", "xsetroot", "ffmpeg"]
               if shutil.which(tool) is None]
    if missing or not (repo / "build/torchlight-gtk").exists():
        raise SystemExit(f"record_demo: needs {', '.join(missing) or 'a build (make)'}")
    # Outside /home, which the namespace replaces with the fixture.
    with tempfile.TemporaryDirectory(prefix="torchlight-demo-", dir="/tmp") as temporary:
        base = Path(temporary)
        (base / "empty").mkdir()
        fixture(base)
        subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "--",
                        sys.executable, __file__, "--namespace", str(base), "--ids",
                        f"{os.getuid()}:{os.getgid()}", "--fps", str(args.fps)], check=True)
        if args.raw:
            shutil.copy(base / "raw.mkv", args.raw)
        convert(base, (repo / args.output).resolve())
