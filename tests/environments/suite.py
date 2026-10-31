"""Run one container's required checks and write a machine-readable result."""
import argparse
from contextlib import contextmanager, nullcontext
import json
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import tempfile
import time


def commands(lane, output):
    """Return named checks; every listed check must execute successfully."""
    if lane == "checks":
        return [("test", ["make", "test"]), ("lint", ["make", "lint"])]
    if lane == "x11":
        return [("popup-actions", ["python3", "tests/run_popup_checks.py", "--actions",
                                    "--screen", "1280x800x24", "--output", str(output / "popup-actions.json")]),
                ("popup-matrix", ["python3", "tests/run_popup_checks.py", "--matrix",
                                   "--output", str(output / "popup-matrix.json")]),
                ("popup-wide-a11y", ["python3", "tests/run_popup_checks.py", "--a11y",
                                      "--screen", "1280x800x24", "--output",
                                      str(output / "popup-wide.json")])]
    if lane == "gnome":
        return [("gnome-wayland", ["python3", "tests/test_windows_gnome_shell.py"])]
    desktop = {"kde": "kwin", "cinnamon": "cinnamon"}[lane]
    return [(f"{desktop}-wayland", ["python3", "tests/test_windows_desktops.py",
                                    "--desktop", desktop])]


@contextmanager
def system_bus(output):
    """Give desktops a private system bus, without host services or elevated privileges."""
    with tempfile.TemporaryDirectory(prefix="torchlight-system-bus-") as directory:
        config = Path(directory) / "bus.conf"
        config.write_text(f"""<busconfig><type>system</type>
<listen>unix:path={directory}/bus</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*"/>
<allow receive_sender="*"/><allow own="*"/></policy></busconfig>""")
        with (output / "system-bus.log").open("w") as log:
            process = subprocess.Popen(["dbus-daemon", "--nofork", "--print-address=1",
                                        f"--config-file={config}"], stdout=subprocess.PIPE,
                                       stderr=log, text=True)
            try:
                with selectors.DefaultSelector() as ready:
                    ready.register(process.stdout, selectors.EVENT_READ)
                    if not ready.select(timeout=10):
                        raise RuntimeError("Private system bus did not start")
                    address = process.stdout.readline().strip()
                if not address:
                    raise RuntimeError("Private system bus returned no address")
                yield {"DBUS_SYSTEM_BUS_ADDRESS": address}
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                process.stdout.close()


def run_check(name, command, output, timeout, reject_skips=True, environment=None):
    """Capture one process group; timeouts terminate its descendants as well."""
    started = time.monotonic()
    case = output / name
    case.mkdir()
    captures = case / "screenshots"
    captures.mkdir()
    env = dict(os.environ, TORCHLIGHT_TEST_REQUIRED="1",
               TORCHLIGHT_TEST_ARTIFACTS=str(case / "sessions"),
               TORCHLIGHT_TEST_VIEW_CAPTURE=str(captures), PYTHONUNBUFFERED="1")
    env.update(environment or {})
    result = dict(name=name, command=command, status="failed", exit_code=None)
    with (case / "output.log").open("w") as log:
        try:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                result["exit_code"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                result["reason"] = "timeout"
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
                # The parent can exit while a descendant ignores SIGTERM.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
        except OSError as error:
            result["reason"] = str(error)
    text = (case / "output.log").read_text(errors="replace")
    # Unit regressions deliberately exercise optional skips; desktop jobs may never skip.
    skipped = 0
    if reject_skips:
        skipped = len(re.findall(r"(?im)^.*\b(?:check skipped|: skipped)\b.*$", text))
    result.update(seconds=round(time.monotonic() - started, 3), skipped=skipped)
    if result["exit_code"] == 0 and skipped == 0:
        result["status"] = "passed"
    elif skipped:
        result["reason"] = "required checks were skipped"
    print(f"{name}: {result['status']} ({result['seconds']} s)", flush=True)
    return result


def metadata(output):
    """Record the image's actual packages and toolchain, rather than assumed versions."""
    with (output / "versions.log").open("w") as log:
        for command in (["uname", "-a"], ["rustc", "--version"], ["cc", "--version"],
                        ["pkg-config", "--modversion", "gtk4"],
                        ["dpkg-query", "-W", "-f=${Package}\t${Version}\n"]):
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
    source = Path("matrix-source.json")
    if source.exists():
        (output / source.name).write_bytes(source.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lane", choices=("checks", "x11", "gnome", "kde", "cinnamon"))
    parser.add_argument("--output", type=Path, default=Path("/artifacts"))
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(lane=args.lane, status="failed", cases=[])
    try:
        metadata(args.output)
        bus = nullcontext({}) if args.lane == "checks" else system_bus(args.output)
        with bus as environment:
            for name, command in commands(args.lane, args.output):
                case = run_check(name, command, args.output, args.timeout,
                                 reject_skips=args.lane != "checks", environment=environment)
                case["screenshots"] = [str(path.relative_to(args.output)) for path in
                                       sorted((args.output / name / "screenshots").glob("*.png"))]
                if args.lane != "checks" and case["status"] == "passed" and not case["screenshots"]:
                    case.update(status="failed", reason="native view screenshots are missing")
                report["cases"].append(case)
        if report["cases"] and all(case["status"] == "passed" for case in report["cases"]):
            report["status"] = "passed"
    except Exception as error:
        report["reason"] = str(error)
    finally:
        (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
