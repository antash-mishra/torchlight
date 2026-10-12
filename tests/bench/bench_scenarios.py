"""M7 scenario benchmark: one isolated headless torchlightd per phase over a
corpus materialized as empty files on disk, with per-thread CPU, wakeups, I/O
and RSS from /proc, and optional sampler profiles (tests/bench/profiler).

Corpus: the M1 synthetic fixture (--fixture BIN --size N) or a NUL-separated
path list (--paths FILE, e.g. BENCH_PATHS) mirrored below the work directory.
A path is a directory when another listed path lies below it; everything else
becomes an empty file. Results append to --results as JSON lines.

The daemon sees the host's installed applications (XDG_DATA_DIRS, unless
--desktop empty) and keeps history (unless --no-history), as on a desktop;
everything else (HOME, XDG_* homes, database, config, socket) is isolated.
CPU is reported per thread name (tl-index, tl-persist, tl-desktop, ...).

Phases (default all, in this order):
  cold         cold index into an empty database
  warm         restart on the saved database, then idle with the default
               periodic settings for --idle-s seconds (full scans, CPU, I/O)
  quiet        idle with periodic rescans off (the resident floor)
  queries      query latency idle and during back-to-back forced full rescans
  lag          single-file create/delete lag in a small and the largest folder
  events       on one daemon: steady RSS before and after the corpus queries
               (cold and warm), 20k files in 2k new folders, rename and
               delete that tree, one file appended and closed 20 times a
               second for 30 s, then the memory gates (ADR 0044): free heap a
               malloc_trim still returns (with the sampler), and RSS once a
               catalog growth past the delta bound rebuilt the base engine,
               against the warm steady RSS
  mounts       in a private user and mount namespace (unshare, nsenter):
               tmpfs mounted and unmounted outside the root, then inside it;
               only the latter may cause full scans
  rescan       forced no-change full scans (reconcile), one at a time: scan
               time, CPU per thread, file traffic, SQLite temp files open
               during the scan, and the kernel watch count (steady and peak)
  syscalls     the daemon launched under strace -f -c: once only to start,
               once with forced no-change full scans; the difference per
               scan by system call (attaching needs ptrace rights we lack)
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
# Catalog growth, in batches, until the delta bound forces a full rebuild.
COMPACT_BATCH, COMPACT_BATCHES = 1000, 64
# Mount and unmount pairs outside the root in the mounts phase.
MOUNT_CYCLES, MOUNT_PAUSE_S = 20, 0.2
# Memory gates (ADR 0044): free heap a trim still returns, and RSS after a
# rebuild against the warm steady RSS.
RETAINED_FREE_GATE_MIB, REBUILT_GROWTH_GATE_PCT = 5, 10.0
# Without XDG_DATA_DIRS the XDG specification's default applies.
DEFAULT_DATA_DIRS = "/usr/local/share:/usr/share"
LAG_ITERATIONS, LAG_PAUSE_S = 15, 0.3
QUERY_COUNT = 900
# Forced no-change full scans measured one by one, and under strace.
RESCAN_REPEATS, STRACE_SCANS = 5, 3
# How often the rescan phase samples watches and open files during a scan.
SCAN_SAMPLE_S = 0.05


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

def data_dirs(options):
    """XDG_DATA_DIRS for the daemon: the host's, or None for an empty directory."""
    if options.desktop != "host":
        return None
    return os.environ.get("XDG_DATA_DIRS") or DEFAULT_DATA_DIRS


def desktop_files(directories):
    """Count .desktop files the daemon's catalog will read below directories."""
    count = 0
    for directory in directories.split(":"):
        for _, _, names in os.walk(Path(directory) / "applications"):
            count += sum(name.endswith(".desktop") for name in names)
    return count


class Daemon:
    def __init__(self, options, name, tree, rescan_ms=None, fresh=False, extra=(), namespace=False,
                 wrapper=()):
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
                   "--max-entries", str(options.max_entries)]
        if options.no_history:
            command.append("--no-history")
        if rescan_ms is not None:
            command += ["--rescan-ms", str(rescan_ms)]
        command += list(extra) + options.daemon_args
        env = dict(os.environ, HOME=str(self.base), XDG_CONFIG_HOME=str(self.base),
                   XDG_DATA_HOME=str(self.base),
                   XDG_DATA_DIRS=data_dirs(options) or str(self.base / "apps"))
        self.profile = options.sampler is not None
        preload = [f"LD_PRELOAD={options.sampler}", f"SAMPLER_CTL={self.fifo}"] if self.profile else []
        # unshare (and env) exec in place, so the pid is the daemon's. The
        # sampler is preloaded only after unshare: its control thread would
        # make unshare's process multithreaded, which CLONE_NEWUSER refuses.
        if namespace:
            command = ["unshare", "--user", "--map-root-user", "--mount", "--propagation",
                       "private", "--", "env", *preload] + command
        elif preload:
            env.update(dict(item.split("=", 1) for item in preload))
        self.results = options.profiles
        self.log = open(self.base / "daemon.log", "ab")
        self.started = time.perf_counter()
        self.process = subprocess.Popen(list(wrapper) + command, stdout=self.log, stderr=self.log,
                                        env=env)
        self.pid = self.process.pid
        # A wrapper (strace) forks the daemon: measure and signal that child.
        self.wrapped = bool(wrapper)
        while self.wrapped and self.pid == self.process.pid:
            children = Path(f"/proc/{self.process.pid}/task/{self.process.pid}/children")
            found = children.read_text().split() if children.exists() else []
            self.pid = int(found[0]) if found else self.pid
            time.sleep(0.01)
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
                out[int(task.name)] = dict(cpu=(int(fields[11]) + int(fields[12])) / HZ, wakeups=switches,
                                           name=(task / "comm").read_text().strip())
            except (FileNotFoundError, IndexError, ProcessLookupError):
                pass
        return out

    def mount(self, target):
        """Mount an empty tmpfs on target inside the daemon's namespace."""
        self._namespace("mount", "-t", "tmpfs", "none", str(target))

    def umount(self, target):
        self._namespace("umount", str(target))

    def _namespace(self, *command):
        subprocess.run(["nsenter", "--preserve-credentials", "--target", str(self.pid), "--user",
                        "--mount", *command], check=True)

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

    def descriptors(self):
        """(inotify watches, open SQLite temp files) from /proc fd tables."""
        watches = temp = 0
        for fd in Path(f"/proc/{self.pid}/fd").iterdir():
            try:
                target = os.readlink(fd)
                if target == "anon_inode:inotify":
                    info = Path(f"/proc/{self.pid}/fdinfo/{fd.name}").read_text()
                    watches += sum(line.startswith("inotify wd:") for line in info.splitlines())
                elif "etilqs" in target:
                    temp += 1
            except OSError:
                pass
        return watches, temp

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
        if self.wrapped:
            os.kill(self.pid, 15)  # the wrapper exits with the daemon
        else:
            self.process.terminate()
        self.process.wait(timeout=60)
        self.log.close()
        self.socket_path.unlink(missing_ok=True)
        # The daemon's singleton lock beside the socket outlives it.
        self.socket_path.with_name(self.socket_path.name + ".lock").unlink(missing_ok=True)


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
        by_name = {}
        for tid, now in self.daemon.threads().items():
            before = self.threads.get(tid, dict(cpu=0, wakeups=0))
            used = now["cpu"] - before["cpu"]
            cpu += used
            wakeups += now["wakeups"] - before["wakeups"]
            by_name[now["name"]] = by_name.get(now["name"], 0.0) + used
        io = self.daemon.io()
        out = dict(wall_s=round(wall, 2), cpu_s=round(cpu, 3), cpu_pct_core=round(100 * cpu / wall, 2),
                   wakeups_per_s=round(wakeups / wall, 1),
                   io_read_mib=round((io["rchar"] - self.io["rchar"]) / 2**20, 1),
                   io_write_mib=round((io["wchar"] - self.io["wchar"]) / 2**20, 1),
                   memory_mib=self.daemon.memory())
        if self.rss:
            out.update(rss_min_mib=min(self.rss), rss_max_mib=max(self.rss))
        out["cpu_s_by_thread"] = {name: round(used, 3) for name, used in
                                  sorted(by_name.items(), key=lambda item: -item[1]) if used > 0}
        return out


def counters(daemon):
    return daemon.status()


def full_scans(status):
    return status["reconciliations"] - status["scoped_reconciliations"]


def forced_scan(daemon, sample=None):
    """Request a full scan and wait for it to finish, calling sample() while
    it runs. Returns the status after it."""
    before = full_scans(daemon.status())
    daemon.call("reconcile")
    while True:
        if sample:
            sample()
        status = daemon.status()
        if full_scans(status) > before and not status["active"]:
            return status
        time.sleep(SCAN_SAMPLE_S)


def strace_counts(path):
    """System call -> calls from an strace -c summary."""
    counts = {}
    for line in Path(path).read_text().splitlines():
        fields = line.split()
        if len(fields) in (5, 6) and fields[0][0].isdigit() and fields[-1] != "total":
            counts[fields[-1]] = int(fields[3])
    return counts


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
        target = self.corpus.inside(f"{self.corpus.big}/zqchurn.log")
        target.touch()
        daemon.wait_present(target.name, target)
        daemon.wait_idle()
        daemon.control("reset")
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

    def warm_up(self, daemon):
        """Run the corpus queries (the kinds the event probes run), so steady
        RSS includes the query scratch pages every searched daemon touches."""
        for query in self.corpus.queries:
            daemon.call(query=query, limit=10)
        time.sleep(RELEASE_SETTLE_S)

    def rebuild(self, daemon):
        """Grow the catalog in batches until the live delta passes its bound
        and the base engine is rebuilt without the deleted entries, then
        warm up. Returns (RSS in MiB, entries added)."""
        builds = daemon.status()["full_builds"]
        compact = self.corpus.inside("zzcompact")
        shutil.rmtree(compact, ignore_errors=True)
        added = 0
        for batch in range(COMPACT_BATCHES):
            folder = compact / f"batch{batch}"
            folder.mkdir(parents=True)
            for i in range(COMPACT_BATCH):
                (folder / f"entry{i}_{batch}.txt").touch()
            added += COMPACT_BATCH + 1
            last = folder / f"entry{COMPACT_BATCH - 1}_{batch}.txt"
            daemon.wait_present(last.name, last)
            daemon.wait_idle()
            if daemon.status()["full_builds"] > builds:
                break
        self.warm_up(daemon)
        return daemon.memory(), added

    def events(self):
        """Burst, rename, delete and churn on one daemon, then the memory
        gates against the warm steady RSS it had before them (ADR 0044)."""
        daemon = self.daemon()
        daemon.wait_idle()
        time.sleep(1)
        steady_cold = daemon.memory()
        self.warm_up(daemon)
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
        trim = self.trim(daemon)
        rebuilt, added = self.rebuild(daemon)
        growth = lambda now, base: round(100 * (now["VmRSS"] - base["VmRSS"]) / base["VmRSS"], 1)
        retained = None if trim is None else trim[0] - trim[1]
        self.record("memory_after_events", steady_cold_mib=steady_cold, steady_mib=steady,
                    after_mib=after, growth_pct=growth(after, steady),
                    growth_vs_cold_pct=growth(after, steady_cold),
                    trim_rss_before_after_mib=trim, retained_free_mib=retained,
                    rebuilt_mib=rebuilt, rebuilt_added_entries=added,
                    rebuilt_growth_pct=growth(rebuilt, steady),
                    gates=dict(retained_free=None if retained is None else retained <= RETAINED_FREE_GATE_MIB,
                               rebuilt_growth=growth(rebuilt, steady) <= REBUILT_GROWTH_GATE_PCT))
        shutil.rmtree(self.corpus.inside("zzcompact"), ignore_errors=True)
        daemon.wait_idle()
        daemon.stop()

    def mounts(self):
        """Mount-table changes in a private namespace: outside the root they
        must cause no scan; inside it, one full scan each (reason "mount")."""
        if shutil.which("unshare") is None or shutil.which("nsenter") is None:
            self.record("mounts", skipped="unshare/nsenter unavailable")
            return
        daemon = Daemon(self.options, self.label, self.corpus.tree, rescan_ms=None, namespace=True)
        daemon.wait_idle()
        outside = daemon.base / "outside-mount"
        outside.mkdir(exist_ok=True)
        before, meter = counters(daemon), Meter(daemon)
        for _ in range(MOUNT_CYCLES):
            daemon.mount(outside)
            time.sleep(MOUNT_PAUSE_S)
            daemon.umount(outside)
            time.sleep(MOUNT_PAUSE_S)
        daemon.wait_idle()
        after = counters(daemon)
        self.record("mounts_outside_root", cycles=MOUNT_CYCLES,
                    mount_changes=after.get("mount_changes", 0) - before.get("mount_changes", 0),
                    **scan_counts(before, after), **meter.result())
        inside = self.corpus.inside("zzmount")
        inside.mkdir(exist_ok=True)
        before, meter = after, Meter(daemon)
        for change in (daemon.mount, daemon.umount):
            change(inside)
            time.sleep(MOUNT_PAUSE_S)
            daemon.wait_idle(minimum_reconciliations=counters(daemon)["reconciliations"] + 1)
        after = counters(daemon)
        self.record("mounts_inside_root",
                    mount_changes=after.get("mount_changes", 0) - before.get("mount_changes", 0),
                    **scan_counts(before, after), **meter.result())
        daemon.dump(f"{self.label}-mounts")
        daemon.stop()
        inside.rmdir()

    def rescan(self):
        """Forced full scans that find nothing changed (ADR 0040)."""
        daemon = self.daemon()
        daemon.wait_idle()
        time.sleep(1)
        steady_watches, _ = daemon.descriptors()
        daemon.control("reset")
        runs = []
        for _ in range(RESCAN_REPEATS):
            meter, peak = Meter(daemon), dict(watches=0, temp_files=0)

            def sample():
                watches, temp = daemon.descriptors()
                peak["watches"] = max(peak["watches"], watches)
                peak["temp_files"] = max(peak["temp_files"], temp)
            status = forced_scan(daemon, sample)
            runs.append(dict(scan_ms=status["last_scan_ms"], writes=status.get("last_scan_writes"),
                             unseen=status.get("last_scan_unseen"), peak_watches=peak["watches"],
                             temp_files=peak["temp_files"], **meter.result()))
        middle = lambda key: sorted(run[key] for run in runs)[len(runs) // 2]
        self.record("rescan_no_change", runs=runs, steady_watches=steady_watches,
                    median_scan_ms=middle("scan_ms"), median_cpu_s=middle("cpu_s"),
                    median_io_read_mib=middle("io_read_mib"), median_io_write_mib=middle("io_write_mib"),
                    max_peak_watches=max(run["peak_watches"] for run in runs),
                    max_temp_files=max(run["temp_files"] for run in runs))
        daemon.dump(f"{self.label}-rescan")
        daemon.stop()

    def syscalls(self):
        """System calls per no-change full scan, from two strace -c runs."""
        if shutil.which("strace") is None:
            self.record("syscalls_per_scan", skipped="strace unavailable")
            return
        totals = {}
        for scans in (0, STRACE_SCANS):
            summary = self.options.work / f"strace-{self.label}-{scans}.txt"
            daemon = Daemon(self.options, self.label, self.corpus.tree, rescan_ms=QUIET_RESCAN_MS,
                            wrapper=["strace", "-f", "-c", "-o", str(summary)])
            daemon.wait_idle()
            for _ in range(scans):
                forced_scan(daemon)
            daemon.stop()
            totals[scans] = strace_counts(summary)
        names = set(totals[0]) | set(totals[STRACE_SCANS])
        per_scan = {name: round((totals[STRACE_SCANS].get(name, 0) - totals[0].get(name, 0)) / STRACE_SCANS)
                    for name in names}
        self.record("syscalls_per_scan", scans=STRACE_SCANS,
                    calls={name: n for name, n in sorted(per_scan.items(), key=lambda item: -item[1])
                           if n > 0})


PHASES = ["cold", "warm", "quiet", "queries", "lag", "events", "mounts", "rescan", "syscalls"]


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
    parser.add_argument("--desktop", choices=("host", "empty"), default="host",
                        help="applications the daemon sees: the host's (default) or none")
    parser.add_argument("--no-history", action="store_true",
                        help="run the daemon without history (no retention prune, no usage)")
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
    directories = data_dirs(options)
    header = dict(scenario="header", corpus=options.corpus_name, files=len(corpus.files),
                  directories=len(corpus.directories), daemon=str(options.daemon),
                  profiled=options.sampler is not None, kernel=os.uname().release,
                  daemon_args=options.daemon_args, phases=options.phases,
                  desktop=options.desktop, desktop_files=desktop_files(directories) if directories else 0,
                  history=not options.no_history)
    with open(options.results, "a") as out:
        out.write(json.dumps(header) + "\n")
    print(json.dumps(header), flush=True)
    scenarios = Scenarios(options, corpus)
    for phase in [p for p in PHASES if p in options.phases]:
        print(f"## {phase}", file=sys.stderr, flush=True)
        getattr(scenarios, phase)()


if __name__ == "__main__":
    main()
