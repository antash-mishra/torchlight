"""Boat-only IPC comparison: feature settings, catalog readiness and stalled refresh.

Run after building torchlightd-release and bench_fixture, on an otherwise idle VM.
Uses the existing synthetic corpus and deterministic Flatpak fixture; no downloads.
"""
import argparse
import json
import os
import socket
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_search_actions import fake_flatpak, wait_for
from bench_daemon import Resident, percentiles, seed_catalog


def timings(service, queries, enabled, legacy=False):
    engine, ipc = [], []
    for query in queries:
        fields = {} if legacy else dict(version=2, mode="local", web_fallback=enabled,
                                       software_suggestions=enabled)
        response, elapsed = service.call(query=query, limit=10, **fields)
        assert len(response.get("actions", [])) == int(enabled)
        engine.append(response["timing"]["engine_us"] / 1000)
        ipc.append(elapsed)
    return dict(engine_ms=percentiles(engine), ipc_ms=percentiles(ipc))


def cpu_ticks(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])


def measure(binary, fixture, size, enabled, legacy=False):
    with tempfile.TemporaryDirectory(prefix="torchlight-actions-bench-") as directory:
        base = Path(directory)
        fake_flatpak(base)
        live = base / "live"
        live.mkdir()
        (live / "needle.txt").touch()
        settings = "" if legacy else (
            f"web_search = {'true' if enabled else 'false'}\n"
            f"software_search = {'auto' if enabled else 'false'}\n")
        (base / "config").write_text(f"root = {base}/virtual\nroot = {live}\n" + settings)
        paths, queries = base / "paths", base / "queries"
        subprocess.run([fixture, str(size), str(paths), str(queries)], check=True)
        whole = [json.loads(line)["query"] for line in queries.read_text().splitlines()]
        typing = [query[:end] for query in whole for end in range(1, len(query) + 1)]
        env = dict(os.environ, HOME=str(base), XDG_DATA_HOME=str(base),
                   XDG_DATA_DIRS=str(base / "empty"), XDG_CONFIG_HOME=str(base),
                   FLATPAK_USER_DIR=str(base / "flatpak-user"),
                   FLATPAK_SYSTEM_DIR=str(base / "flatpak-system"),
                   FLATPAK_CONFIG_DIR=str(base / "flatpak-config"),
                   M9_FIXTURE=str(base), PATH=f"{base / 'bin'}:{os.environ['PATH']}")
        service = Resident(binary, base, size, environment=env)
        try:
            service.start()
            service.idle()
            service.stop()
            seed_catalog(service, paths.read_bytes().split(b"\0")[:-1])
            if enabled:
                (base / "stall").touch()
            startup = service.start()
            cold = timings(service, typing[:100], enabled, legacy)
            if enabled:
                # Restart after the controlled cold probe instead of awaiting 20 s.
                service.stop()
                (base / "stall").unlink()
                service.start()
                wait_for(lambda: service.call("status")[0]["software"]["state"] == "ready")
            service.idle()
            timings(service, typing, enabled, legacy)
            ready = timings(service, typing, enabled, legacy)
            memory = service.memory()
            before, start = cpu_ticks(service.process.pid), time.monotonic()
            time.sleep(10)
            idle_cpu = ((cpu_ticks(service.process.pid) - before) / os.sysconf("SC_CLK_TCK")
                        / (time.monotonic() - start) * 100)
            # Idle IPC connections deliberately expire after five seconds.
            service.stream.close()
            service.connection.close()
            service.connection = socket.socket(socket.AF_UNIX)
            service.connection.settimeout(15)
            service.connection.connect(str(service.socket_path))
            service.stream = service.connection.makefile("rb")
            active = None
            if enabled:
                (base / "stall").touch()
                service.call("software_refresh", version=2)
                time.sleep(0.1)
                active = timings(service, typing, enabled)
            return dict(enabled=enabled, legacy=legacy, size=size, startup_ms=round(startup, 3),
                        cold=cold, ready=ready, refreshing=active, memory_kib=memory,
                        idle_cpu_percent=round(idle_cpu, 3))
        finally:
            service.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=50000)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, help="Optional pre-M9 optimized daemon")
    args = parser.parse_args()
    binary = str(Path("build/torchlightd-release").resolve())
    fixture = str(Path("build/bench_fixture").resolve())
    results = []
    runs = [(binary, enabled, False) for enabled in (False, True, True, False)]
    if args.baseline:
        baseline = (str(args.baseline.resolve()), False, True)
        runs = [baseline, *runs, baseline]
    for executable, enabled, legacy in runs:
        results.append(measure(executable, fixture, args.size, enabled, legacy))
        args.output.write_text(json.dumps(results, indent=2) + "\n")
        print(json.dumps(results[-1]), flush=True)
