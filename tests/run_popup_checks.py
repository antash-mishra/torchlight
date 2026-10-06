"""Run GTK acceptance on a private X11 display and D-Bus, leaving the desktop intact."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--xvfb", default="Xvfb")
parser.add_argument("--xdotool", default="xdotool")
parser.add_argument("--wm", default="metacity")
parser.add_argument("--screen", default="800x600x24")
parser.add_argument("--matrix", action="store_true")
parser.add_argument("--a11y", action="store_true")
parser.add_argument("--bench", action="store_true")
parser.add_argument("--output", help="JSON file for acceptance or benchmark measurements")
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="torchlight-gtk-session-") as temporary:
    base = Path(temporary)
    runtime = base / "runtime"
    runtime.mkdir(mode=0o700)
    services = base / "services"
    services.mkdir()
    if args.a11y:
        (services / "org.a11y.Bus.service").symlink_to("/usr/share/dbus-1/services/org.a11y.Bus.service")
    bus_config = base / "bus.conf"
    # Optional portal/secret services can block GTK startup in a private bus.
    # This bus exposes only the services needed by these isolated fixtures.
    bus_config.write_text(f"""<busconfig>
<type>session</type><listen>unix:tmpdir={runtime}</listen><auth>EXTERNAL</auth>
<servicedir>{services}</servicedir>
<policy context="default"><allow send_destination="*"/>
<allow receive_sender="*"/><allow own="*"/></policy>
</busconfig>""")
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), GDK_BACKEND="x11", GSK_RENDERER="cairo")
    if not args.a11y:
        env["GTK_A11Y"] = "none"
    with (base / "xserver.log").open("w") as server_log:
        server = subprocess.Popen([args.xvfb, "-displayfd", "1", "-screen", "0", args.screen,
                                   "-nolisten", "tcp"], stdout=subprocess.PIPE,
                                  stderr=server_log, text=True, env=env)
        try:
            number = server.stdout.readline().strip()
            assert number.isdigit(), (base / "xserver.log").read_text()
            env["DISPLAY"] = ":" + number
            script = "tests/bench/bench_popup.py" if args.bench else "tests/test_popup.py"
            command = [sys.executable, script, "--xdotool", args.xdotool]
            if args.matrix:
                command.append("--matrix")
            if args.a11y:
                command.append("--a11y")
            if args.output:
                command.extend(["--output", str(Path(args.output).resolve())])
            # A single session owns both the WM and launcher; its last child is
            # waited before the display and temporary runtime directory go away.
            wrapper = base / "session.py"
            wrapper.write_text("""import os, subprocess, sys, time
from pathlib import Path
wm = subprocess.Popen([sys.argv[1], '--sm-disable'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    time.sleep(.5)
    if 'tests/bench/bench_popup.py' not in sys.argv and Path('build/test_popup_view').exists():
        subprocess.run(['build/test_popup_view'], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', G_DEBUG='fatal-warnings'))
    raise SystemExit(subprocess.run(sys.argv[2:]).returncode)
finally:
    wm.terminate()
    wm.wait(timeout=10)
""")
            result = subprocess.run(["dbus-run-session", "--config-file=" + str(bus_config), "--",
                                     sys.executable, str(wrapper), args.wm, *command], env=env)
            if result.returncode:
                raise SystemExit(result.returncode)
        finally:
            server.terminate()
            server.wait(timeout=10)
