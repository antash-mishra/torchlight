"""M7 scenario benchmark: one isolated headless torchlightd per phase over a
corpus materialized as empty files on disk, with per-thread CPU, wakeups, I/O
and RSS from /proc, and optional sampler profiles (tests/bench/profiler).

Corpus: the M1 synthetic fixture (--fixture BIN --size N) or a NUL-separated
path list (--paths FILE, e.g. BENCH_PATHS) mirrored below the work directory.
A path is a directory when another listed path lies below it; everything else
becomes an empty file. Results append to --results as JSON lines.

Phases (default all, in this order):
  cold         cold index into an empty database
  warm         restart on the saved database, then idle with the default
               periodic settings for --idle-s seconds (full scans, CPU, I/O)
  quiet        idle with periodic rescans off (the resident floor)
  queries      query latency idle and during back-to-back forced full rescans
  lag          single-file create/delete lag in a small and the largest folder
  events       on one daemon: 20k files in 2k new folders, rename and delete
               that tree, one file appended and closed 20 times a second for
               30 s, then RSS against the daemon's steady RSS before them and,
               with the sampler, malloc_trim to show retained heap
The daemon closes clients idle for five seconds (IPC_DEADLINE_MS), so the
client reconnects when a call fails."""
import argparse
import datetime
import json
import os
from pathlib import Path
import random
import shutil
import socket
import subprocess
import sys
import time

HZ = os.sysconf("SC_CLK_TCK")
PAGE_MIB = 1024
IGNORED_NAMES = ("node_modules", "target", "build", "__pycache__")
QUIET_RESCAN_MS = 3600000
DEFAULT_RESCAN_MS = 30000
BURST_FOLDERS, BURST_FILES_PER_FOLDER, BURST_FOLDERS_PER_PACKAGE = 2000, 10, 100
CHURN_HZ, CHURN_SECONDS = 20, 30
# Longer than the writer's one-second memory-release spacing.
RELEASE_SETTLE_S = 2
LAG_ITERATIONS, LAG_PAUSE_S = 15, 0.3
QUERY_COUNT = 900


def percentiles(values):
    ordered = sorted(values)
    if not ordered:
        return dict(n=0)
    at = lambda fraction: round(ordered[min(len(ordered) - 1, int(len(ordered) * fraction))], 3)
    return dict(n=len(ordered), p50=at(0.5), p95=at(0.95), p99=at(0.99), max=round(ordered[-1], 3))


# ---- corpus -------------------------------------------------------------------

def common_directory(paths):
    prefix = os.path.commonpath([p for p in paths[:1000]] + [paths[-1]])
    while not all(p == prefix or p.startswith(prefix + b"/") for p in paths):
        prefix = prefix.rsplit(b"/", 1)[0] or b"/"
    return prefix


def materialize(path_list, tree):
    """Mirror a NUL-separated absolute path list under tree as empty files and
    directories. Returns (relative path, is_dir) pairs, sorted."""
    paths = [p for p in Path(path_list).read_bytes().split(b"\0") if p.startswith(b"/")]
    base = common_directory(paths)
    directories = set()
    for path in paths:
        parent = path.rsplit(b"/", 1)[0]
        while len(parent) > len(base) and parent not in directories:
            directories.add(parent)
            parent = parent.rsplit(b"/", 1)[0]
    entries = sorted((p[len(base):].lstrip(b"/"), p in directories) for p in paths if len(p) > len(base))
    if not (tree / ".complete").exists():
        partial = tree.with_name(tree.name + ".partial")
        shutil.rmtree(partial, ignore_errors=True)
        partial.mkdir(parents=True)
        root = os.fsencode(partial)
        for relative, is_dir in entries:
            target = root + b"/" + relative
            if is_dir:
                os.makedirs(target, exist_ok=True)
            else:
                os.makedirs(target.rsplit(b"/", 1)[0], exist_ok=True)
                os.close(os.open(target, os.O_CREAT | os.O_WRONLY, 0o644))
        shutil.rmtree(tree, ignore_errors=True)
        partial.rename(tree)
        (tree / ".complete").touch()
    return [(os.fsdecode(r), d) for r, d in entries]


class Corpus:
    def __init__(self, tree, entries, seed=7):
        self.tree = tree
        self.random = random.Random(seed)
        visible = lambda r: not any(part.startswith(".") or part in IGNORED_NAMES for part in r.split("/"))
        self.files = [r for r, d in entries if not d and visible(r)]
        self.directories = [r for r, d in entries if d and visible(r)]
        counts = {}
        for relative, _ in entries:
            parent = relative.rsplit("/", 1)[0] if "/" in relative else ""
            counts[parent] = counts.get(parent, 0) + 1
        self.children = counts
        self.big = max(self.directories, key=lambda d: counts.get(d, 0))
        self.small = [d for d in self.directories if 1 <= counts.get(d, 0) <= 8] or [self.big]
        self.queries = self.make_queries()

    def make_queries(self):
        names = [f.rsplit("/", 1)[-1] for f in self.random.sample(self.files, min(QUERY_COUNT, len(self.files)))]
        queries = []
        for i, name in enumerate(names):
            if i % 3 == 0:
                queries.append(name)                                  # exact name
            elif i % 3 == 1:
                queries.append(name[:self.random.randint(3, 6)])     # typed prefix
            else:
                k = self.random.randrange(len(name)) if len(name) > 4 else 0
                queries.append(name[:k] + name[k + 1:])               # one deletion typo
        return [q for q in queries if q.strip()]

    def inside(self, relative):
        path = (self.tree / relative).resolve()
        assert path.is_relative_to(self.tree.resolve()), relative
        return self.tree / relative


# ---- daemon -------------------------------------------------------------------

class Daemon:
    def __init__(self, options, name, tree, rescan_ms=None, fresh=False, extra=()):
        self.base = options.work / "runs" / name
        if fresh:
            shutil.rmtree(self.base, ignore_errors=True)
        (self.base / "apps").mkdir(parents=True, exist_ok=True)
        (self.base / "config").write_text(f"root = {tree}\n")
        # Absolute and below the 108-byte sun_path limit.
        runtime = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"))
        self.socket_path = runtime / f"tl-scenario-{os.getpid()}-{name}.sock"
        self.fifo = self.base / "sampler.fifo"
        if not self.fifo.exists():
            os.mkfifo(self.fifo)
        command = [str(options.daemon), "--db", str(self.base / "catalog.db"),
                   "--config", str(self.base / "config"), "--socket", str(self.socket_path),
                   "--no-history", "--max-entries", str(options.max_entries)]
        if rescan_ms is not None:
            command += ["--rescan-ms", str(rescan_ms)]
        command += list(extra) + options.daemon_args
        env = dict(os.environ, HOME=str(self.base), XDG_CONFIG_HOME=str(self.base),
                   XDG_DATA_HOME=str(self.base), XDG_DATA_DIRS=str(self.base / "apps"))
        self.profile = options.sampler is not None
        if self.profile:
            env.update(LD_PRELOAD=str(options.sampler), SAMPLER_CTL=str(self.fifo))
        self.results = options.profiles
        self.log = open(self.base / "daemon.log", "ab")
        self.started = time.perf_counter()
        self.process = subprocess.Popen(command, stdout=self.log, stderr=self.log, env=env)
        self.pid = self.process.pid
        self.connection = self.stream = None
        self.sequence = 0
        self.connect()
        self.ready_ms = round((time.perf_counter() - self.started) * 1000)

    def connect(self, timeout=600):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError((self.base / "daemon.log").read_text(errors="replace"))
            connection = socket.socket(socket.AF_UNIX)
            try:
                connection.connect(str(self.socket_path))
                self.connection, self.stream = connection, connection.makefile("rb")
                return
            except (FileNotFoundError, ConnectionRefusedError):
                connection.close()
                time.sleep(0.005)
        raise TimeoutError("daemon socket")

    def call(self, op="query", **fields):
        try:
            return self._call(op, **fields)
        except (BrokenPipeError, ConnectionResetError, json.JSONDecodeError):
            self.stream.close()
            self.connection.close()
            self.connect()
            return self._call(op, **fields)

    def _call(self, op, **fields):
        self.sequence += 1
        request = dict(version=1, request_id=str(self.sequence), op=op, **fields)
        start = time.perf_counter_ns()
        self.connection.sendall(json.dumps(request).encode() + b"\n")
        response = json.loads(self.stream.readline(4 * 1024 * 1024))
        while response.get("phase") != "final":
            response = json.loads(self.stream.readline(4 * 1024 * 1024))
        return response, (time.perf_counter_ns() - start) / 1e6

    def status(self):
        return self.call("status")[0]["indexing"]

    def wait_idle(self, minimum_reconciliations=1, timeout=900, settle_ms=300):
        deadline = time.monotonic() + timeout
        quiet_since = None
        while time.monotonic() < deadline:
            status = self.status()
            if not status["active"] and status["reconciliations"] >= minimum_reconciliations:
                quiet_since = quiet_since or time.monotonic()
                if (time.monotonic() - quiet_since) * 1000 >= settle_ms:
                    return status
            else:
                quiet_since = None
            time.sleep(0.02)
        raise TimeoutError("daemon never went idle")

    def paths(self, query, limit=20):
        response, ms = self.call(query=query, limit=limit)
        return [r.get("path") for r in response.get("results", [])], ms

    def wait_present(self, name, path, present=True, timeout=120):
        start = time.perf_counter()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (str(path) in self.paths(name)[0]) == present:
                return (time.perf_counter() - start) * 1000
            time.sleep(0.005)
        raise TimeoutError(f"{name} present={present}")

    def threads(self):
        out = {}
        for task in Path(f"/proc/{self.pid}/task").iterdir():
            try:
                fields = (task / "stat").read_text().rsplit(")", 1)[1].split()
                status = (task / "status").read_text()
                switches = sum(int(status.split(key)[1].split()[0])
                               for key in ("\nvoluntary_ctxt_switches:", "nonvoluntary_ctxt_switches:"))
                out[int(task.name)] = dict(cpu=(int(fields[11]) + int(fields[12])) / HZ, wakeups=switches)
            except (FileNotFoundError, IndexError, ProcessLookupError):
                pass
        return out

    def io(self):
        values = dict(line.split(": ") for line in Path(f"/proc/{self.pid}/io").read_text().splitlines())
        return {key: int(values[key]) for key in ("rchar", "wchar", "write_bytes")}

    def memory(self):
        values = {}
        for line in Path(f"/proc/{self.pid}/status").read_text().splitlines():
            if line.startswith(("VmRSS:", "VmHWM:", "RssAnon:", "RssFile:")):
                key, value = line.split()[:2]
                values[key[:-1]] = int(value) // PAGE_MIB
        return values

    def reset_peak(self):
        Path(f"/proc/{self.pid}/clear_refs").write_text("5")

    def control(self, command):
        if self.profile:
            with open(self.fifo, "w") as fifo:
                fifo.write(command + "\n")

    def dump(self, name):
        if not self.profile:
            return
        target = self.results / f"{name}.prof"
        target.unlink(missing_ok=True)
        self.control(f"dump {target}")
        for _ in range(500):
            if target.exists() and target.stat().st_size > 0:
                time.sleep(0.1)
                return
            time.sleep(0.01)

    def stop(self):
        if self.connection:
            self.stream.close()
            self.connection.close()
        self.process.terminate()
        self.process.wait(timeout=60)
        self.log.close()
        self.socket_path.unlink(missing_ok=True)


class Meter:
    """CPU, wakeups and I/O over a window; RSS sampled on demand."""

    def __init__(self, daemon):
        self.daemon = daemon
        self.start = time.perf_counter()
        self.threads = daemon.threads()
        self.io = daemon.io()
        self.rss = []

    def sample(self):
        self.rss.append(self.daemon.memory()["VmRSS"])

    def idle_for(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.sample()
            time.sleep(0.1)

    def result(self):
        wall = time.perf_counter() - self.start
        cpu = wakeups = 0.0
        for tid, now in self.daemon.threads().items():
            before = self.threads.get(tid, dict(cpu=0, wakeups=0))
            cpu += now["cpu"] - before["cpu"]
            wakeups += now["wakeups"] - before["wakeups"]
        io = self.daemon.io()
        out = dict(wall_s=round(wall, 2), cpu_s=round(cpu, 3), cpu_pct_core=round(100 * cpu / wall, 2),
                   wakeups_per_s=round(wakeups / wall, 1),
                   io_read_mib=round((io["rchar"] - self.io["rchar"]) / 2**20, 1),
                   io_write_mib=round((io["wchar"] - self.io["wchar"]) / 2**20, 1),
                   memory_mib=self.daemon.memory())
        if self.rss:
            out.update(rss_min_mib=min(self.rss), rss_max_mib=max(self.rss))
        return out


def counters(daemon):
    return daemon.status()


def scan_counts(before, after):
    full = lambda s: s["reconciliations"] - s["scoped_reconciliations"]
    out = dict(full_scans=full(after) - full(before),
               scoped_scans=after["scoped_reconciliations"] - before["scoped_reconciliations"],
               delta_publications=after["delta_publications"] - before["delta_publications"],
               full_builds=after["full_builds"] - before["full_builds"],
               overflows=after["watch_overflows"] - before["watch_overflows"], watches=after["watches"])
    for key in ("repair_scopes", "last_full_reason"):
        if key in after:
            out[key] = after[key]
    return out


# ---- phases -------------------------------------------------------------------

class Scenarios:
    def __init__(self, options, corpus):
        self.options, self.corpus = options, corpus
        self.label = options.corpus_name

    def record(self, name, **data):
        row = dict(scenario=f"{self.label}/{name}", **data)
        with open(self.options.results, "a") as out:
            out.write(json.dumps(row) + "\n")
        print(json.dumps(row), flush=True)

    def daemon(self, rescan_ms=QUIET_RESCAN_MS, fresh=False):
        return Daemon(self.options, self.label, self.corpus.tree, rescan_ms=rescan_ms, fresh=fresh)

    def cold(self):
        daemon = self.daemon(fresh=True)
        meter = Meter(daemon)
        status = daemon.wait_idle()
        self.record("cold_index", socket_ready_ms=daemon.ready_ms, index_ms=status["last_scan_ms"],
                    watches=status["watches"], **meter.result())
        daemon.dump(f"{self.label}-cold")
        daemon.stop()

    def warm(self):
        daemon = self.daemon(rescan_ms=None)
        meter = Meter(daemon)
        status = daemon.wait_idle()
        self.record("warm_start", socket_ready_ms=daemon.ready_ms, startup_scan_ms=status["last_scan_ms"],
                    **meter.result())
        daemon.dump(f"{self.label}-warm")
        time.sleep(1)
        daemon.reset_peak()
        before = counters(daemon)
        meter = Meter(daemon)
        meter.idle_for(self.options.idle_s)
        self.record("idle_default", rescan_ms=DEFAULT_RESCAN_MS, **scan_counts(before, counters(daemon)),
                    **meter.result())
        daemon.dump(f"{self.label}-idle")
        daemon.stop()

    def quiet(self):
        daemon = self.daemon()
        daemon.wait_idle()
        time.sleep(1)
        daemon.control("reset")
        meter = Meter(daemon)
        meter.idle_for(60)
        self.record("idle_quiet", rescan_ms=QUIET_RESCAN_MS, **meter.result())
        daemon.dump(f"{self.label}-quiet")
        daemon.stop()

    def queries(self):
        daemon = self.daemon()
        daemon.wait_idle()
        times = [daemon.call(query=q, limit=10)[1] for q in self.corpus.queries]
        self.record("query_idle", round_trip_ms=percentiles(times))
        times, rescans, i = [], 0, 0
        meter = Meter(daemon)
        while len(times) < len(self.corpus.queries) and i < 50 * len(self.corpus.queries):
            if not daemon.status()["active"]:
                daemon.call("reconcile")
                rescans += 1
                time.sleep(0.02)
            response, ms = daemon.call(query=self.corpus.queries[i % len(self.corpus.queries)], limit=10)
            if response.get("indexing", {}).get("active"):
                times.append(ms)
            i += 1
        self.record("query_during_full_scan", round_trip_ms=percentiles(times), full_scans=rescans,
                    **meter.result())
        daemon.dump(f"{self.label}-queries")
        daemon.stop()

    def lag_series(self, daemon, directory, tag):
        create, delete = [], []
        for i in range(LAG_ITERATIONS):
            name = f"zqprobe{tag}{i}x.txt"
            path = self.corpus.inside(f"{directory}/{name}")
            time.sleep(LAG_PAUSE_S)
            path.touch()
            create.append(daemon.wait_present(name, path))
            time.sleep(LAG_PAUSE_S)
            path.unlink()
            delete.append(daemon.wait_present(name, path, present=False))
        return dict(create_ms=percentiles(create), delete_ms=percentiles(delete))

    def lag(self):
        daemon = self.daemon()
        daemon.wait_idle()
        daemon.control("reset")
        small = self.corpus.random.choice(self.corpus.small)
        before = counters(daemon)
        self.record("lag_small_dir", dir_entries=self.corpus.children.get(small, 0),
                    **self.lag_series(daemon, small, "s"), **scan_counts(before, counters(daemon)))
        before = counters(daemon)
        self.record("lag_big_dir", dir_entries=self.corpus.children.get(self.corpus.big, 0),
                    **self.lag_series(daemon, self.corpus.big, "b"), **scan_counts(before, counters(daemon)))
        daemon.dump(f"{self.label}-lag")
        daemon.stop()

    def event(self, daemon, name, action, probe, present=True):
        daemon.wait_idle()
        daemon.control("reset")
        daemon.reset_peak()
        before = counters(daemon)
        meter = Meter(daemon)
        start = time.perf_counter()
        action()
        acted = time.perf_counter()
        visible = daemon.wait_present(probe.name, probe, present=present)
        daemon.wait_idle()
        self.record(name, action_ms=round((acted - start) * 1000), visible_ms=round(visible),
                    **scan_counts(before, counters(daemon)), **meter.result())
        daemon.dump(f"{self.label}-{name}")

    def churn(self, daemon):
        daemon.wait_idle()
        daemon.control("reset")
        target = self.corpus.inside(f"{self.corpus.big}/zqchurn.log")
        before = counters(daemon)
        meter = Meter(daemon)
        writes, end = 0, time.monotonic() + CHURN_SECONDS
        while time.monotonic() < end:
            with open(target, "a") as out:
                out.write("x")
            writes += 1
            time.sleep(1 / CHURN_HZ)
        daemon.wait_idle()
        self.record("churn_big_dir_20hz", dir_entries=self.corpus.children.get(self.corpus.big, 0),
                    writes=writes, **scan_counts(before, counters(daemon)), **meter.result())
        daemon.dump(f"{self.label}-churn")
        target.unlink()
        daemon.wait_idle()

    def trim(self, daemon):
        """malloc_trim(0) through the sampler: RSS before and after, in MiB."""
        if not daemon.profile:
            return None
        report = self.options.profiles / f"{self.label}-trim.txt"
        report.unlink(missing_ok=True)
        daemon.control(f"trim {report}")
        for _ in range(300):
            if report.exists() and report.read_text().strip():
                return [int(v) // PAGE_MIB for v in report.read_text().split()]
            time.sleep(0.01)
        return None

    def events(self):
        """Burst, rename, delete and churn on one daemon, then its RSS against
        the steady RSS it had before them."""
        daemon = self.daemon()
        daemon.wait_idle()
        time.sleep(1)
        steady = daemon.memory()
        burst, moved = self.corpus.inside("zzburst"), self.corpus.inside("zzmoved")
        shutil.rmtree(burst, ignore_errors=True)
        shutil.rmtree(moved, ignore_errors=True)
        last = f"pkg{(BURST_FOLDERS - 1) // BURST_FOLDERS_PER_PACKAGE}/mod{BURST_FOLDERS - 1}/" \
               f"file{BURST_FILES_PER_FOLDER - 1}_{BURST_FOLDERS - 1}.c"

        def create():
            for i in range(BURST_FOLDERS):
                folder = burst / f"pkg{i // BURST_FOLDERS_PER_PACKAGE}" / f"mod{i}"
                folder.mkdir(parents=True)
                for j in range(BURST_FILES_PER_FOLDER):
                    (folder / f"file{j}_{i}.c").touch()
        self.event(daemon, "burst_create_20k", create, burst / last)
        self.event(daemon, "rename_20k_tree", lambda: burst.rename(moved), moved / last)
        self.event(daemon, "delete_20k_tree", lambda: shutil.rmtree(moved), moved / last, present=False)
        self.churn(daemon)
        time.sleep(RELEASE_SETTLE_S)
        after = daemon.memory()
        self.record("memory_after_events", steady_mib=steady, after_mib=after,
                    growth_pct=round(100 * (after["VmRSS"] - steady["VmRSS"]) / steady["VmRSS"], 1),
                    trim_rss_before_after_mib=self.trim(daemon))
        daemon.stop()


PHASES = ["cold", "warm", "quiet", "queries", "lag", "events"]


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--daemon", required=True, type=Path)
    parser.add_argument("--sampler", type=Path, help="sampler.so: profile every phase")
    parser.add_argument("--fixture", type=Path, help="bench_fixture binary for the synthetic corpus")
    parser.add_argument("--size", type=int, default=500000, help="synthetic corpus size")
    parser.add_argument("--paths", type=Path, help="NUL-separated path list to mirror instead")
    parser.add_argument("--work", type=Path, default=Path("build/bench-scenarios"))
    parser.add_argument("--results", type=Path)
    parser.add_argument("--label", default="scenarios", help="results file label")
    parser.add_argument("--idle-s", type=float, default=95)
    parser.add_argument("--max-entries", type=int, default=600000)
    parser.add_argument("--daemon-arg", dest="daemon_args", action="append", default=[],
                        help="extra torchlightd argument (repeatable)")
    parser.add_argument("phases", nargs="*", default=PHASES)
    options = parser.parse_args()
    if (options.fixture is None) == (options.paths is None):
        parser.error("give exactly one of --fixture or --paths")
    unknown = set(options.phases) - set(PHASES)
    if unknown:
        parser.error(f"unknown phases {sorted(unknown)}")
    options.daemon, options.work = options.daemon.resolve(), options.work.resolve()
    if options.sampler:
        options.sampler = options.sampler.resolve()
    today = datetime.date.today().isoformat()
    options.results = (options.results or Path(f"tests/bench/results/{today}-m7-{options.label}.jsonl")).resolve()
    options.profiles = options.work / "profiles"
    options.profiles.mkdir(parents=True, exist_ok=True)
    return options


def main():
    options = parse_arguments()
    if options.paths:
        options.corpus_name, path_list = "paths", options.paths.resolve()
    else:
        options.corpus_name = f"fx{options.size // 1000}"
        path_list = options.work / f"{options.corpus_name}.paths"
        if not path_list.exists():
            subprocess.run([str(options.fixture), str(options.size), str(path_list),
                            str(options.work / f"{options.corpus_name}.queries")], check=True)
    tree = options.work / "trees" / options.corpus_name
    corpus = Corpus(tree, materialize(path_list, tree))
    header = dict(scenario="header", corpus=options.corpus_name, files=len(corpus.files),
                  directories=len(corpus.directories), daemon=str(options.daemon),
                  profiled=options.sampler is not None, kernel=os.uname().release,
                  daemon_args=options.daemon_args, phases=options.phases)
    with open(options.results, "a") as out:
        out.write(json.dumps(header) + "\n")
    print(json.dumps(header), flush=True)
    scenarios = Scenarios(options, corpus)
    for phase in [p for p in PHASES if p in options.phases]:
        print(f"## {phase}", file=sys.stderr, flush=True)
        getattr(scenarios, phase)()


if __name__ == "__main__":
    main()
