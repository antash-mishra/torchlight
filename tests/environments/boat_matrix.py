"""Orchestrate desktop tests on Boat; the local machine only transfers and reports files."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import shlex
import signal
import subprocess
import tarfile
import threading
import time

LANES = ("x11", "gnome", "kde", "cinnamon")
REMOTE = "/home/user/torchlight"
WORKER = f"{REMOTE}/tests/environments/worker.py"


def json_lines(text):
    """Decode the CLI's JSONL, rejecting unexpected output instead of losing an id."""
    return [json.loads(line) for line in text.splitlines() if line.strip()]


def source_archive(root, output):
    """Archive current tracked and unignored files, including uncommitted edits."""
    paths = subprocess.check_output(["git", "ls-files", "--cached", "--others",
                                     "--exclude-standard", "-z"], cwd=root).split(b"\0")
    digest = hashlib.sha256()
    archive = output / "source.tar.gz"
    with tarfile.open(archive, "w:gz", dereference=False) as bundle:
        for raw in sorted(set(paths)):
            if not raw:
                continue
            relative = Path(raw.decode("utf-8", "surrogateescape"))
            path = root / relative
            if not path.exists() and not path.is_symlink():
                continue  # A tracked deletion belongs to this working tree too.
            if not path.is_file() and not path.is_symlink():
                raise ValueError(f"Unsupported source entry: {relative}")
            digest.update(raw + b"\0" + str(path.lstat().st_mode).encode() + b"\0")
            digest.update(str(path.readlink()).encode() if path.is_symlink() else path.read_bytes())
            bundle.add(path, arcname=str(relative), recursive=False)
        source = dict(commit=subprocess.check_output(["git", "rev-parse", "HEAD"],
                                                     cwd=root, text=True).strip(),
                      source_sha256=digest.hexdigest())
        content = (json.dumps(source, indent=2) + "\n").encode()
        info = tarfile.TarInfo("matrix-source.json")
        info.size = len(content)
        bundle.addfile(info, io.BytesIO(content))
    (output / "source.json").write_text(json.dumps(source, indent=2) + "\n")
    return archive, source


class Boat:
    """Own only this run's sandboxes; journal creation before waiting for readiness."""

    def __init__(self, executable, output):
        self.executable = executable
        self.output = output
        self.owned = []

    def call(self, arguments, timeout=120):
        """Call a bounded read/control operation and decode its last JSON object."""
        result = subprocess.run([self.executable, "--no-update", "--json", *arguments],
                                capture_output=True, text=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f"Boat {arguments[0]} failed: {result.stderr or result.stdout}")
        values = json_lines(result.stdout)
        if not values:
            raise RuntimeError(f"Boat {arguments[0]} returned no JSON")
        return values[-1]

    def journal(self):
        (self.output / "sandboxes.json").write_text(json.dumps(self.owned, indent=2) + "\n")

    def create(self, arguments):
        """Record an allocated id even if provisioning subsequently fails."""
        sandbox = None
        ready = False
        with (self.output / "provision.log").open("a") as log:
            process = subprocess.Popen([self.executable, "--no-update", *arguments, "--json"],
                                       stdout=subprocess.PIPE, stderr=log, text=True)
            deadline = threading.Timer(300, process.kill)
            deadline.start()
            try:
                for line in process.stdout:
                    log.write(line)
                    log.flush()
                    event = json.loads(line)
                    ready = ready or event.get("event") == "ready"
                    if event.get("id") and event.get("event") in ("created", "ready"):
                        sandbox = event["id"]
                        if sandbox not in self.owned:
                            self.owned.append(sandbox)
                            self.journal()
                if process.wait() or sandbox is None or not ready:
                    raise RuntimeError("Sandbox creation failed or timed out; see provision.log")
            finally:
                deadline.cancel()
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                process.stdout.close()
        return sandbox

    def transfer(self, source, target):
        """Copy only explicit paths; a failure is retained in the overall report."""
        subprocess.run([self.executable, "--no-update", "scp", "-r", str(source), str(target)],
                       check=True, timeout=300, stdout=subprocess.DEVNULL)

    def start(self, sandbox, command):
        # Pass shell syntax explicitly; Boat otherwise quotes a single command as one word.
        result = self.call(["exec", sandbox, "--detach", "--", "bash", "-lc", command])
        return result["pid"]

    def wait(self, sandbox, pid, label):
        """Wait for an already detached job; leave full output in a local log."""
        deadline = time.monotonic() + 7200
        failures = 0
        while time.monotonic() < deadline:
            try:
                status = self.call(["exec", sandbox, "--status", str(pid)])
                failures = 0
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                failures += 1
                if failures >= 6:
                    raise RuntimeError(f"{label}: status polling failed six times: {error}") from error
                print(f"{label}: status unavailable, retrying ({failures}/6)", flush=True)
                time.sleep(5)
                continue
            if status.get("known") is False:
                raise RuntimeError(f"{label}: Boat lost process {pid}")
            if not status.get("running", False):
                with (self.output / f"{label}-remote.log").open("w") as log:
                    log.write(status.get("stdout", "") + status.get("stderr", ""))
                return status.get("exitCode") == 0
            time.sleep(5)
        raise TimeoutError(f"{label}: cloud job exceeded two hours")

    def snapshot(self, sandbox):
        """Fork only after stop's asynchronous disk snapshot is complete."""
        result = self.call(["stop", sandbox])
        operation = result["operation"]["id"]
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            state = self.call(["stop", sandbox, "--status", operation])["stop"]["status"]
            if state == "completed":
                return
            if state in ("failing", "failed"):
                raise RuntimeError("Base snapshot failed")
            time.sleep(3)
        raise TimeoutError("Base snapshot did not complete in ten minutes")

    def cleanup(self):
        """Delete only journaled sandboxes; keep failed deletions visible for recovery."""
        failed = []
        for sandbox in reversed(self.owned):
            for attempt in range(3):
                try:
                    self.call(["delete", sandbox, "--yes"])
                    break
                except Exception as error:
                    if attempt == 2:
                        failed.append(dict(id=sandbox, error=str(error)))
                    else:
                        time.sleep(2)
        (self.output / "cleanup.json").write_text(json.dumps(failed, indent=2) + "\n")
        return failed


def prepare(boat, root, output, lanes, template=None):
    """Build the selected images and baseline checks once before making worker forks."""
    archive, source = source_archive(root, output)
    arguments = (["fork", template, "--wait"] if template else
                 ["new", "--name", "torchlight-matrix-base"])
    base = boat.create([*arguments, "--no-env", "--ttl", "10800"])
    print(f"Base {base}: building {', '.join(lanes)} and running test/lint", flush=True)
    boat.transfer(archive, f"{base}:/home/user/matrix-source.tar.gz")
    # A template may contain an older checkout/results; delete only these paths on our new fork.
    command = (f"rm -rf -- {REMOTE} /home/user/matrix-build /home/user/matrix-results && "
               f"mkdir -p {REMOTE} && tar -xzf /home/user/matrix-source.tar.gz -C {REMOTE} && " +
               shlex.join(["python3", WORKER, "build", "--output", "/home/user/matrix-build",
                           "--lanes", *lanes]))
    pid = boat.start(base, command)
    try:
        success = boat.wait(base, pid, "build")
    finally:
        boat.transfer(f"{base}:/home/user/matrix-build", output / "build")
    if not success:
        raise RuntimeError("Image build or baseline checks failed; see build artifacts")
    boat.snapshot(base)
    return base, source


def collect(boat, sandbox, pid, lane, output):
    """Collect a job independently, including artifacts from a failing test."""
    error = None
    success = False
    try:
        success = boat.wait(sandbox, pid, lane)
    except Exception as failure:
        error = str(failure)
    try:
        boat.transfer(f"{sandbox}:/home/user/matrix-results/{lane}", output / lane)
        report = json.loads((output / lane / "result.json").read_text())
        expected = 4 if lane == "x11" else 1
        valid = (report.get("lane") == lane and len(report.get("cases", [])) == expected and
                 all(case.get("status") == "passed" and case.get("skipped") == 0
                     for case in report["cases"]))
        if not success or not valid:
            report["status"] = "failed"
        if error:
            report["reason"] = error
        return report
    except Exception as failure:
        reason = f"{error}; {failure}" if error else str(failure)
        return dict(lane=lane, status="failed", reason=reason, cases=[])


def execute(boat, base, lanes, workers, output):
    """Start each wave's jobs before waiting, with one isolated VM per lane."""
    reports = []
    for offset in range(0, len(lanes), workers):
        prepared, pending = [], []
        for lane in lanes[offset:offset + workers]:
            try:
                sandbox = boat.create(["fork", base, "--wait", "--no-env", "--no-snapshots",
                                       "--ttl", "10800"])
                prepared.append((sandbox, lane))
            except Exception as error:
                reports.append(dict(lane=lane, status="failed", reason=str(error), cases=[]))
        # Let every VM finish provisioning before starting this wave's tests.
        for sandbox, lane in prepared:
            try:
                command = shlex.join(["python3", WORKER, "run", "--lane", lane,
                                      "--output", "/home/user/matrix-results"])
                pid = boat.start(sandbox, command)
                print(f"{lane}: started on {sandbox}", flush=True)
                pending.append((sandbox, pid, lane))
            except Exception as error:
                reports.append(dict(lane=lane, status="failed", reason=str(error), cases=[]))
        for sandbox, pid, lane in pending:
            report = collect(boat, sandbox, pid, lane, output)
            reports.append(report)
            print(f"{lane}: {report['status']}", flush=True)
    return reports


def interrupted(signum, frame):
    """Route termination through the same cleanup path as Ctrl+C."""
    raise KeyboardInterrupt(f"Interrupted by signal {signum}")


def write_report(output, report):
    """Write both structured results and a short report without hiding skipped jobs."""
    report["finished"] = datetime.now(timezone.utc).isoformat()
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = ["# Torchlight desktop matrix", "", f"Status: **{report['status']}**", "",
             "| Environment | Result | Passed checks | Skipped |", "|---|---|---|---|"]
    for lane in report["lanes"]:
        cases = lane.get("cases", [])
        passed = sum(case.get("status") == "passed" for case in cases)
        skipped = sum(case.get("skipped", 0) for case in cases)
        lines.append(f"| {lane['lane']} | {lane['status']} | {passed} | {skipped} |")
    if report.get("error"):
        lines.extend(["", report["error"]])
    if report.get("cleanup_failures"):
        lines.extend(["", "Cleanup needs attention; see cleanup.json for sandbox ids."])
    (output / "report.md").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lanes", nargs="+", choices=LANES, default=list(LANES))
    parser.add_argument("--workers", type=int, choices=range(1, 5), default=4)
    parser.add_argument("--boat", default="boat", help="Boat CLI executable")
    parser.add_argument("--template", help="Fork a stopped sandbox to reuse its Docker cache; never delete it")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    lanes = list(dict.fromkeys(args.lanes))
    root = Path(__file__).resolve().parents[2]
    output = (args.output or root / "build/desktop-matrix" /
              datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")).resolve()
    if output.exists():
        parser.error("output must be a new directory, so results cannot overwrite an earlier run")
    output.mkdir(parents=True)
    boat = Boat(args.boat, output)
    report = dict(status="failed", started=datetime.now(timezone.utc).isoformat(), lanes=[])
    signal.signal(signal.SIGTERM, interrupted)
    try:
        limits = boat.call(["limits"])
        if not limits.get("canStart"):
            raise RuntimeError(f"Boat cannot start workers: {limits.get('startBlockedReason')}")
        base, report["source"] = prepare(boat, root, output, lanes, args.template)
        report["lanes"] = execute(boat, base, lanes, args.workers, output)
        if len(report["lanes"]) == len(lanes) and all(
                lane["status"] == "passed" for lane in report["lanes"]):
            report["status"] = "passed"
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = str(error) or "Interrupted"
    finally:
        report["cleanup_failures"] = boat.cleanup()
        if report["cleanup_failures"]:
            report["status"] = "failed"
        write_report(output, report)
    print(f"Report: {output / 'report.md'}", flush=True)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
