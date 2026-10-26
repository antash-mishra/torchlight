"""Run one launcher over the comparison's app and file queries, inside its container.

    run_compare.py materialize
        Create the synthetic home (empty files) from the exported corpus.
    run_compare.py run LAUNCHER --round N --out FILE [--smoke]
        Ask LAUNCHER every query and record, per query, the rank of the first right
        answer among what the launcher shows and the time the answer took.

Every launcher gets the same inputs from /opt/compare (see Dockerfile.base): the
app fixture through XDG_DATA_DIRS, the corpus paths, the held-out file queries and
the hand-written everyday queries. Each one runs with its own defaults; the adapters
below only translate a query into the launcher's own call and its answer into rows.
"""
import argparse
import base64
import json
import os
import random
import re
import subprocess
import sys
import time
from pathlib import Path

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

DATA = Path("/opt/compare")
HOME = Path("/home/user")
PATHS_FILE = DATA / "paths"  # NUL-separated, from build/bench_fixture
FILE_QUERIES = DATA / "queries.jsonl"  # held-out seed 2, with target indexes
EVERYDAY_QUERIES = DATA / "everyday_queries.tsv"
APP_SEED = 2  # held out like the file queries; nothing was tuned on it
APP_PREFIX_LETTERS = 3
MIN_TYPO_WORD = 5  # as QUERY_MIN_TYPO_WORD in tests/bench/queries.c
SHOWN = 10  # rows Torchlight's popup asks for (POPUP_RESULTS); ranks beyond count as misses
WARMUP = 20  # untimed queries per section before timing, for every launcher
SMOKE = 12  # queries per section in a smoke run
DISPLAY = ":99"
# Folder names Torchlight's crawler skips by default (ignored_name in src/fs/crawl.c,
# ADR 0062), besides every name starting with a dot. Queries for files inside them
# are kept and marked, so the report can show both counts.
SKIPPED_FOLDERS = {b"node_modules", b"bower_components", b"site-packages", b"dist-packages",
                   b"target", b"build", b"CMakeFiles", b"__pycache__"}


def load_apps():
    """The fixture's applications as GLib shows them in a GNOME session."""
    apps = [app for app in Gio.AppInfo.get_all() if app.should_show()]
    return sorted(apps, key=lambda app: app.get_id())


def apply_edit(word, rng):
    """One random edit at a position >= 1, the same four kinds as apply_edit in
    tests/bench/queries.c: substitution, adjacent swap, deletion, insertion."""
    chars = list(word)
    position = 1 + rng.randrange(len(chars) - 1)
    letter = chr(ord("a") + rng.randrange(26))
    kind = rng.randrange(4)

    def substitute():
        replacement = letter if letter != chars[position] else chr(ord("a") + (ord(letter) - ord("a") + 1) % 26)
        chars[position] = replacement

    if kind == 0:
        substitute()
    elif kind == 1:
        if position + 1 < len(chars) and chars[position] != chars[position + 1]:
            chars[position], chars[position + 1] = chars[position + 1], chars[position]
        else:
            substitute()
    elif kind == 2:
        del chars[position]
    else:
        chars.insert(position, letter)
    return "".join(chars)


def app_typo(name, rng):
    words = list(re.finditer(r"[a-z]+", name))
    if not words:
        return None
    longest = max(words, key=lambda match: len(match.group()))
    if len(longest.group()) < MIN_TYPO_WORD:
        return None
    return name[:longest.start()] + apply_edit(longest.group(), rng) + name[longest.end():]


def app_queries(apps):
    """Generated known-item queries: the whole name, its first letters, one typo.
    Any app with the same name counts as right, like duplicate file names do."""
    rng = random.Random(APP_SEED)
    ids_by_name = {}
    for app in apps:
        ids_by_name.setdefault(app.get_name().lower(), set()).add(app.get_id())
    queries = []
    for app in apps:
        name = app.get_name().lower()
        right = sorted(ids_by_name[name])
        queries.append(dict(kind="name", query=name, right=right))
        if len(name) > APP_PREFIX_LETTERS:
            queries.append(dict(kind="prefix", query=name[:APP_PREFIX_LETTERS], right=right))
        typo = app_typo(name, rng)
        if typo is not None:
            queries.append(dict(kind="typo", query=typo, right=right))
    return queries


def everyday_queries(apps):
    known = {app.get_id() for app in apps}
    queries = []
    for line in EVERYDAY_QUERIES.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        query, right = line.split("\t")
        right = right.split()
        missing = [desktop_id for desktop_id in right if desktop_id not in known]
        if missing:
            raise SystemExit(f"everyday query {query!r} names unknown apps {missing}")
        queries.append(dict(kind="everyday", query=query, right=right))
    return queries


def load_paths():
    return PATHS_FILE.read_bytes().split(b"\0")[:-1]


def parent_name(path):
    return path.rsplit(b"/", 2)[-2] if path.count(b"/") >= 2 else b""


def file_queries(paths):
    queries = []
    for line in FILE_QUERIES.read_text().splitlines():
        record = json.loads(line)
        target = paths[record["target"]]
        folders = target.split(b"/")[1:-1]
        skipped = any(name in SKIPPED_FOLDERS or name.startswith(b".") for name in folders)
        queries.append(dict(kind=record["kind"], query=record["query"], skipped_folder=skipped,
                            basename=os.path.basename(target), parent=parent_name(target)))
    return queries


def right_answer(row, query, section):
    """Whether one shown row answers the query. Rows are (kind, key): ("app", desktop
    id) or ("file", path bytes). File relevance is query_relevant's rule."""
    kind, key = row
    if section in ("apps", "everyday"):
        return kind == "app" and key in query["right"]
    if kind != "file" or os.path.basename(key) != query["basename"]:
        return False
    return query["kind"] != "parent" or parent_name(key) == query["parent"]


def materialize():
    """Empty files and folders at the corpus's paths, so crawling launchers see the
    same home that the others get as a list, and a stub for each app's program."""
    paths = load_paths()
    folders = {os.path.dirname(path) for path in paths}
    for path in paths:
        if path in folders:
            os.makedirs(path, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "ab").close()
    print(f"materialized {len(paths)} paths under {HOME}")
    print(f"stubbed {stub_programs()} app programs")


def stub_programs():
    """GLib only loads an entry whose Exec program exists, as it does on the desktop
    the fixture came from. Create a do-nothing program for each missing one."""
    created = 0
    for entry in sorted((DATA / "apps" / "applications").glob("*.desktop")):
        keys = GLib.KeyFile()
        keys.load_from_file(str(entry), GLib.KeyFileFlags.NONE)
        _, argv = GLib.shell_parse_argv(keys.get_string("Desktop Entry", "Exec"))
        program = Path(argv[0]) if argv[0].startswith("/") else Path("/usr/local/bin") / argv[0]
        if GLib.find_program_in_path(argv[0]) is None and not program.exists():
            program.parent.mkdir(parents=True, exist_ok=True)
            program.write_text("#!/bin/sh\nexit 0\n")
            program.chmod(0o755)
            created += 1
    return created


# ---------------------------------------------------------------- adapters


class Torchlight:
    """The release daemon, asked over its socket as the popup asks, and its rows put
    in the popup's order: groups (apps, settings, folders, files) placed where their
    best row ranks, rows in rank order within a group (ui/gtk/model.c)."""
    name = "torchlight"
    sections = ("apps", "everyday", "files")
    binary = "/usr/local/bin/torchlightd"

    def __init__(self, apps, paths):
        self.size = len(paths)
        self.service = None

    def start(self):
        sys.path.insert(0, str(DATA))
        from bench_daemon import Resident
        base = Path("/var/tl")
        base.mkdir(exist_ok=True)
        (base / "config").write_text(f"root = {HOME}\n")
        environment = dict(os.environ, XDG_CONFIG_HOME=str(base / "config-home"),
                           XDG_CACHE_HOME=str(base / "cache"), XDG_STATE_HOME=str(base / "state"))
        self.service = Resident(self.binary, base, self.size, environment=environment)
        self.service.start()
        self.service.idle()
        deadline = time.monotonic() + 60
        while not any(kind == "app" for kind, _ in self.query("terminal", "apps")):
            if time.monotonic() > deadline:
                raise TimeoutError("the desktop catalog never listed Terminal")
            time.sleep(0.1)

    def query(self, text, section):
        response, _ = self.service.call(query=text, limit=SHOWN)
        rows = []
        for result in response["results"]:
            if result.get("desktop_id"):
                # Settings is its own group in the popup but still an app entry.
                rows.append(("app", result["desktop_id"], result["kind"]))
            else:
                path = os.fsencode(result["path"]) if "path" in result else base64.b64decode(result["path_b64"])
                rows.append(("file" if result["kind"] in ("file", "folder") else result["kind"], path, result["kind"]))
        return popup_order(rows)

    def details(self):
        return dict(memory_kib=self.service.memory())

    def stop(self):
        self.service.close()

    def version(self):
        output = subprocess.run([self.binary, "--version"], capture_output=True, text=True)
        commit = (DATA / "commit").read_text().strip()
        return f"{output.stdout.strip()} ({commit})"


def popup_order(rows):
    def group(row):
        return row[2] if len(row) > 2 else row[0]
    order = []
    for row in rows:
        if group(row) not in order:
            order.append(group(row))
    return [row[:2] for g in order for row in rows if group(row) == g]


class Gnome:
    """GNOME Shell's app search: GLib's g_desktop_app_info_search, the call
    Shell.AppSystem.search makes, with Shell's visibility filter. With no usage
    history, Shell's per-group sort keeps GLib's order."""
    name = "gnome"
    sections = ("apps", "everyday")

    def __init__(self, apps, paths):
        self.visible = {app.get_id() for app in apps}

    def start(self):
        pass

    def query(self, text, section):
        return [("app", desktop_id) for group in Gio.DesktopAppInfo.search(text)
                for desktop_id in group if desktop_id in self.visible]

    def details(self):
        return {}

    def stop(self):
        pass

    def version(self):
        return f"GLib {GLib.MAJOR_VERSION}.{GLib.MINOR_VERSION}.{GLib.MICRO_VERSION} (as in GNOME Shell 46)"


class Ulauncher:
    """Ulauncher's own app search (AppDb.find): its SortedList scores every app with
    fuzzy_search.get_score on the name and 0.8x on the executable, keeps scores of
    at least 75 and shows the best 9."""
    name = "ulauncher"
    sections = ("apps", "everyday")

    class Item:
        def __init__(self, desktop_id, search):
            self.desktop_id, self.search = desktop_id, search
            self.score = 0

        def get_search_name(self):
            return self.search

    def __init__(self, apps, paths):
        self.apps = apps
        self.items = []

    def start(self):
        from ulauncher.search.apps.AppDb import get_exec_name, search_name
        from ulauncher.search.SortedList import SortedList
        from ulauncher.utils.desktop.reader import find_apps
        self.sorted_list = SortedList
        # Ulauncher's own reader and order: equal scores (capped at 100) keep the
        # order its database got the apps in, which is the order it found the
        # files on disk.
        for app in find_apps():  # the record AppDb.put_app builds for each
            name = app.get_string("X-GNOME-FullName") or app.get_name()
            exec_name = get_exec_name(app.get_string("Exec") or "")
            desktop_id = os.path.basename(app.get_filename())
            self.items.append(self.Item(desktop_id, search_name(name, exec_name)))
        if len(self.items) != len(self.apps):
            raise SystemExit(f"Ulauncher found {len(self.items)} of the {len(self.apps)} apps")

    def query(self, text, section):
        results = self.sorted_list(text, min_score=75, limit=9)
        for item in self.items:
            results.append(item)
        return [("app", item.desktop_id) for item in results]

    def details(self):
        return {}

    def stop(self):
        pass

    def version(self):
        return "Ulauncher 5.16.2"


class LineFilter:
    """Base for launchers that filter a list of lines given on stdin."""

    def __init__(self, apps, paths):
        # A line launcher shows app names; the tab-separated id is only for scoring.
        self.app_lines = "".join(f"{app.get_name()}\t{app.get_id()}\n" for app in apps).encode()
        self.file_lines = b"".join(path + b"\n" for path in paths if b"\n" not in path)

    def rows(self, output, section):
        lines = output.splitlines()[:SHOWN]
        if section == "files":
            return [("file", line) for line in lines]
        return [("app", line.split(b"\t")[-1].decode()) for line in lines]

    def details(self):
        return {}

    def stop(self):
        pass


class Fzf(LineFilter):
    """fzf --filter with its defaults (extended fuzzy search, smart case, sorted by
    score); app lines match on the name field only."""
    name = "fzf"
    sections = ("apps", "everyday", "files")

    def start(self):
        pass

    def query(self, text, section):
        command = ["fzf", "--filter", text]
        if section != "files":
            command += ["--delimiter", "\t", "--nth", "1"]
        stdin = self.file_lines if section == "files" else self.app_lines
        result = subprocess.run(command, input=stdin, capture_output=True)
        return self.rows(result.stdout, section)

    def version(self):
        return "fzf " + subprocess.run(["fzf", "--version"], capture_output=True, text=True).stdout.split()[0]


class Rofi:
    """rofi under Xvfb with its default matching ("normal", no sort). Only dmenu mode
    can print its results without a window (-dump), so apps get what drun matches by
    default: one line per app of its name, generic name, command, categories and
    keywords (drun-match-fields), in drun's order (by name). -i ignores case, as drun
    does; -format i prints line numbers, so nothing but those fields is matched.
    -dump prints matches in input order and ignores -sort, which is also what the
    window shows by default, so rofi's sorted fuzzy setting cannot be measured."""
    name = "rofi"
    sections = ("apps", "everyday", "files")

    def __init__(self, apps, paths):
        self.display = None
        by_name = sorted(apps, key=lambda app: app.get_name().lower())
        self.app_ids = [app.get_id() for app in by_name]
        self.app_lines = "".join(self.drun_fields(app) + "\n" for app in by_name).encode()
        self.paths = [path for path in paths if b"\n" not in path]
        self.file_lines = b"".join(path + b"\n" for path in self.paths)

    @staticmethod
    def drun_fields(app):
        fields = [app.get_name(), app.get_generic_name() or "", app.get_string("Exec") or "",
                  app.get_categories() or "", ";".join(app.get_keywords() or [])]
        return " ".join(field for field in fields if field)

    def start(self):
        self.display = start_display()

    def query(self, text, section):
        command = ["rofi", "-dmenu", "-i", "-format", "i", "-filter", text, "-dump"]
        stdin = self.file_lines if section == "files" else self.app_lines
        result = subprocess.run(command, input=stdin, capture_output=True,
                                env=dict(os.environ, DISPLAY=DISPLAY))
        indexes = [int(line) for line in result.stdout.split()[:SHOWN]]
        if section == "files":
            return [("file", self.paths[index]) for index in indexes]
        return [("app", self.app_ids[index]) for index in indexes]

    def details(self):
        return {}

    def stop(self):
        self.display.terminate()
        self.display.wait()

    def version(self):
        return subprocess.run(["rofi", "-version"], capture_output=True, text=True).stdout.strip()


class Plocate:
    """plocate over its own index of the home (built when the image was built), every
    word a case-insensitive pattern that must match, as `plocate -i a b` does."""
    name = "plocate"
    sections = ("files",)
    database = "/var/lib/compare-plocate.db"

    def __init__(self, apps, paths):
        pass

    def start(self):
        pass

    def query(self, text, section):
        command = ["plocate", "-d", self.database, "-i", "-l", str(SHOWN), "--"] + text.split()
        result = subprocess.run(command, capture_output=True)
        return [("file", line) for line in result.stdout.splitlines()[:SHOWN]]

    def details(self):
        return {}

    def stop(self):
        pass

    def version(self):
        return subprocess.run(["plocate", "--version"], capture_output=True, text=True).stdout.splitlines()[0]


def start_display():
    display = subprocess.Popen(["Xvfb", DISPLAY, "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 10
    while not Path(f"/tmp/.X11-unix/X{DISPLAY[1:]}").exists():
        if time.monotonic() > deadline:
            raise TimeoutError("Xvfb did not start")
        time.sleep(0.05)
    return display


def adapters(launcher, apps, paths):
    known = dict(torchlight=Torchlight, gnome=Gnome, ulauncher=Ulauncher, rofi=Rofi, fzf=Fzf,
                 plocate=Plocate)
    return [known[launcher](apps, paths)]


# ---------------------------------------------------------------- running


def rank_of(rows, query, section):
    for position, row in enumerate(rows[:SHOWN], 1):
        if right_answer(row, query, section):
            return position
    return None


def shown_label(row):
    kind, key = row
    return key if isinstance(key, str) else os.path.basename(key).decode(errors="replace")


def run_section(adapter, section, queries):
    for query in queries[:WARMUP]:
        adapter.query(query["query"], section)
    records = []
    for query in queries:
        start = time.perf_counter_ns()
        rows = adapter.query(query["query"], section)
        elapsed_ms = (time.perf_counter_ns() - start) / 1e6
        record = dict(kind=query["kind"], query=query["query"], rank=rank_of(rows, query, section),
                      ms=round(elapsed_ms, 4), shown=[shown_label(row) for row in rows[:3]])
        if query.get("skipped_folder"):
            record["skipped_folder"] = True
        records.append(record)
    return records


def summary(records):
    found_first = sum(record["rank"] == 1 for record in records)
    top_five = sum(record["rank"] is not None and record["rank"] <= 5 for record in records)
    times = sorted(record["ms"] for record in records)
    return (f"{len(records)} queries, first {found_first / len(records):.0%}, "
            f"top 5 {top_five / len(records):.0%}, median {times[len(times) // 2]:.2f} ms")


def run(launcher, round_number, out, smoke):
    apps = load_apps()
    fixture = len(list((DATA / "apps" / "applications").glob("*.desktop")))
    if len(apps) != fixture:
        raise SystemExit(f"GLib shows {len(apps)} of the fixture's {fixture} apps")
    paths = load_paths()
    sections = dict(apps=app_queries(apps), everyday=everyday_queries(apps), files=file_queries(paths))
    if smoke:
        sections = {name: queries[:SMOKE] for name, queries in sections.items()}
    results = []
    for adapter in adapters(launcher, apps, paths):
        started = time.perf_counter()
        adapter.start()
        start_s = time.perf_counter() - started
        entry = dict(launcher=adapter.name, version=adapter.version(), round=round_number,
                     start_s=round(start_s, 3), sections={})
        for section in adapter.sections:
            entry["sections"][section] = run_section(adapter, section, sections[section])
            print(f"{adapter.name} {section}: {summary(entry['sections'][section])}", flush=True)
        entry.update(adapter.details())
        adapter.stop()
        results.append(entry)
    corpus = dict(apps=len(apps), paths=len(paths))
    Path(out).write_text(json.dumps(dict(corpus=corpus, results=results), indent=1) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("materialize")
    runner = commands.add_parser("run")
    runner.add_argument("launcher", choices=["torchlight", "gnome", "ulauncher", "rofi", "fzf", "plocate"])
    runner.add_argument("--round", type=int, required=True)
    runner.add_argument("--out", required=True)
    runner.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    if args.command == "materialize":
        materialize()
    else:
        run(args.launcher, args.round, args.out, args.smoke)


if __name__ == "__main__":
    main()
