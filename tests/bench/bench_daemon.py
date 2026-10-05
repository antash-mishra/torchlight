"""Resident SQLite/IPC benchmark using M1's corpus and held-out query generator."""
import argparse
import json
import os
from pathlib import Path
import platform
import socket
import sqlite3
import subprocess
import tempfile
import time


def percentiles(values):
    ordered = sorted(values)
    if not ordered:
        return {"n": 0}
    def at(fraction):
        return round(ordered[min(len(ordered) - 1, int(len(ordered) * fraction))], 3)
    return dict(n=len(ordered), p50=at(0.5), p95=at(0.95), p99=at(0.99), maximum=round(ordered[-1], 3))


class Resident:
    def __init__(self, binary, base, size, model=None):
        self.base, self.size = base, size
        self.socket_path = base / "socket"
        self.database = base / "catalog.db"
        self.config = base / "config"
        self.process = None
        self.connection = None
        self.stream = None
        self.log = tempfile.TemporaryFile()
        self.sequence = 0
        self.command = [binary, "--db", str(self.database), "--config", str(self.config),
                        "--socket", str(self.socket_path), "--no-history", "--rescan-ms", "3600000",
                        "--max-entries", str(size + 10000)]
        self.model = model
        if model:
            self.command += ["--model", model, "--semantic-deadline-ms", "1000"]

    def start(self):
        start = time.perf_counter()
        self.process = subprocess.Popen(self.command, stdout=self.log, stderr=self.log,
                                        env=dict(os.environ, XDG_CONFIG_HOME=str(self.base),
                                                 XDG_DATA_HOME=str(self.base), HOME=str(self.base),
                                                 XDG_DATA_DIRS=str(self.base / "no-system-apps")))
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log.seek(0)
                raise RuntimeError(self.log.read().decode(errors="replace"))
            connection = socket.socket(socket.AF_UNIX)
            connection.settimeout(15)
            try:
                connection.connect(str(self.socket_path))
                self.connection = connection
                self.stream = connection.makefile("rb")
                return (time.perf_counter() - start) * 1000
            except (FileNotFoundError, ConnectionRefusedError):
                connection.close()
                time.sleep(0.005)
        raise TimeoutError("daemon startup")

    def call(self, operation="query", **fields):
        self.sequence += 1
        request = dict(version=1, request_id=str(self.sequence), op=operation, **fields)
        start = time.perf_counter_ns()
        self.connection.sendall(json.dumps(request).encode() + b"\n")
        response = json.loads(self.stream.readline(1024 * 1024))
        initial = response
        lexical_ms = (time.perf_counter_ns() - start) / 1e6
        while response["phase"] != "final":
            response = json.loads(self.stream.readline(1024 * 1024))
        elapsed = (time.perf_counter_ns() - start) / 1e6
        response["lexical_elapsed_ms"] = lexical_ms
        for field in ("indexing", "timing"):
            if field not in response and field in initial:
                response[field] = initial[field]
        assert response["status"] == "ok", response
        assert response["phase"] == "final" and response["request_id"] == request["request_id"]
        return response, elapsed

    def idle(self):
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            response, _ = self.call("status")
            state = response["indexing"]
            if not state["active"] and state["reconciliations"]:
                return state
            time.sleep(0.02)
        raise TimeoutError("initial reconciliation")

    def memory(self):
        values = {}
        for line in Path(f"/proc/{self.process.pid}/status").read_text().splitlines():
            if line.startswith(("VmRSS:", "VmHWM:")):
                key, value, _ = line.split()
                values[key[:-1]] = int(value)
        return values

    def stop(self):
        if self.stream:
            self.stream.close()
            self.stream = None
        if self.connection:
            self.connection.close()
            self.connection = None
        if self.process:
            self.process.terminate()
            self.process.wait(timeout=30)
            self.log.seek(0)
            assert self.process.returncode == 0, self.log.read().decode(errors="replace")
            self.process = None

    def close(self):
        self.stop()
        self.log.close()


def seed_catalog(service, paths):
    # Fixture SQL only. Schema creation/migrations were performed by store.c.
    prefix = b"/home/user"
    virtual = os.fsencode(service.base / "virtual")
    with sqlite3.connect(service.database) as connection:
        for path in paths:
            path = virtual + path[len(prefix):]
            name = path.rsplit(b"/", 1)[-1]
            connection.execute("INSERT OR IGNORE INTO files(path,name,is_dir,mtime,size) VALUES(?,?,?,0,0)",
                               (path, name, int(path == virtual)))
        connection.execute("INSERT OR IGNORE INTO roots VALUES(?)", (virtual,))
        connection.execute("UPDATE meta SET value='1' WHERE key='catalog_gen'")
        return connection.execute("SELECT count(*) FROM files").fetchone()[0]


def benchmark(binary, fixture, size, model=None):
    with tempfile.TemporaryDirectory(prefix="torchlight-ipc-bench-") as temporary:
        base = Path(temporary)
        live = base / "live"
        live.mkdir()
        for i in range(100):
            (live / f"initial-{i}.txt").touch()
        (base / "config").write_text(f"root = {base}/virtual\nroot = {live}\n")
        path_file, query_file = base / "paths", base / "queries"
        subprocess.run([fixture, str(size), str(path_file), str(query_file)], check=True)
        paths = path_file.read_bytes().split(b"\0")[:-1]
        queries = [json.loads(line)["query"] for line in query_file.read_text().splitlines()]
        service = Resident(binary, base, size, model)
        try:
            service.start()  # Initialize schema and the real 100-file scan.
            initial = service.idle()
            service.stop()
            entries = seed_catalog(service, paths)
            del paths
            startup = service.start()
            first, first_ms = service.call(query="projectNotes.md", limit=10)
            service.idle()
            semantic_ready_ms = None
            if model:
                ready_start = time.perf_counter()
                deadline = time.monotonic() + 600
                while time.monotonic() < deadline:
                    response, _ = service.call(query="projectNotes.md", limit=10)
                    if response['emb_gen'] is not None:
                        semantic_ready_ms = (time.perf_counter() - ready_start) * 1000
                        break
                    time.sleep(.05)
                else:
                    raise TimeoutError('semantic publication')
            steady_memory = service.memory()
            round_trip, engine, lexical_phase = [], [], []
            for query in queries:
                response, elapsed = service.call(query=query, limit=10)
                lexical_phase.append(response['lexical_elapsed_ms'])
                round_trip.append(elapsed)
                engine.append(response["timing"]["engine_us"] / 1000)
            # Measure a real filesystem batch while rebuilding a large resident
            # catalog. Only the 100 live files are scanned; virtual paths remain.
            start = time.perf_counter()
            for i in range(100):
                (live / f"update-{i}.txt").touch()
            marker = live / "arrival.txt"
            marker.touch()
            changed = time.perf_counter()
            during, during_engine = [], []
            index = 0
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                response, elapsed = service.call(query=queries[index % len(queries)], limit=10)
                if response["indexing"]["active"]:
                    during.append(elapsed)
                    during_engine.append(response["timing"]["engine_us"] / 1000)
                result, _ = service.call(query="arrival.txt", limit=10)
                if any(row.get("path") == str(marker) for row in result["results"]):
                    update_lag = (time.perf_counter() - changed) * 1000
                    break
                index += 1
            else:
                raise TimeoutError("update publication")
            latest = service.idle()
            output = dict(requested_paths=size, catalog_entries=entries, held_out_queries=len(queries),
                          semantic_ready_ms=semantic_ready_ms, lexical_phase_ms=percentiles(lexical_phase),
                          startup_ms=round(startup, 3), first_query_ms=round(first_ms, 3),
                          first_engine_ms=first["timing"]["engine_us"] / 1000,
                          engine_ms=percentiles(engine), round_trip_ms=percentiles(round_trip),
                          indexing_engine_ms=percentiles(during_engine), indexing_round_trip_ms=percentiles(during),
                          update_lag_ms=round(update_lag, 3),
                          initial_100_files_reconcile_ms=initial["last_scan_ms"],
                          update_reconcile_ms=latest["last_scan_ms"],
                          initial_memory_kib=steady_memory, after_update_memory_kib=service.memory(),
                          file_batch_creation_ms=round((changed - start) * 1000, 3))
            print(json.dumps(output, sort_keys=True), flush=True)
        finally:
            service.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("daemon")
    parser.add_argument("fixture")
    parser.add_argument("--sizes", nargs="+", type=int, default=[50000, 500000])
    parser.add_argument("--model")
    args = parser.parse_args()
    cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), "unknown")
    print(json.dumps(dict(cpu=cpu, platform=platform.platform(), load_average=os.getloadavg(),
                          ram_kib=os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE") // 1024,
                          build="C17 -O3 -DNDEBUG", corpus="M1 synthetic seed 42, deduplicated; held-out query seed 2")), flush=True)
    for size in args.sizes:
        benchmark(str(Path(args.daemon).resolve()), str(Path(args.fixture).resolve()), size,
                  str(Path(args.model).resolve()) if args.model else None)
