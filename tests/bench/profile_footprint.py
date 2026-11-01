"""Attribute daemon heap use on synthetic catalogs, with optional app discovery.

Run in Boat. Reports uninstrumented PSS/CPU separately from Valgrind Massif's
allocation trees. Catalog paths are synthetic and deliberately unavailable;
this isolates resident indexes rather than measuring filesystem/watch costs.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from bench_daemon import Resident, seed_catalog

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_search_actions import Service, fake_flatpak


def memory(pid):
    keys = {"Rss", "Pss", "Pss_Anon", "Private_Dirty", "Swap"}
    return {line.split(":")[0]: int(line.split()[1])
            for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
            if line.split(":")[0] in keys}


def cpu(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return sum(int(fields[i]) for i in (11, 12, 13, 14)) / os.sysconf("SC_CLK_TCK")


def ready(service, software):
    service.idle()
    if not software:
        return
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        status, _ = service.call("status")
        if status["software"]["state"] == "ready":
            return
        time.sleep(.1)
    raise AssertionError("synthetic software catalog did not become ready")


def workload(service, queries, software):
    for query in queries[:40]:
        service.call(query=query)
    if software:
        service.call(version=2, mode="software", query="Fixture App", limit=10)


def profile(args, size, software):
    name = f"{size}-software-{int(software)}"
    with tempfile.TemporaryDirectory(prefix="torchlight-heap-") as temporary:
        base = Path(temporary)
        fake_flatpak(base)
        fixture_service = Service(base)
        env = fixture_service.env
        fixture_service.log.close()
        flatpak = base / "bin/flatpak"
        script = flatpak.read_text().replace('flathub\\t\\ntesting\\t', 'flathub\\t')
        script = script.replace(
            'print("app/org.example.Editor/x86_64/stable\\tExample Editor\\tEdit source code")',
            'for i in range(4000): print(f"app/org.example.Fixture{i}/x86_64/stable\\tFixture App {i}\\tSynthetic app")')
        flatpak.write_text(script)
        discovery = "auto" if software else "false"
        (base / "config").write_text(f"root = {base}/virtual\nroot = {fixture_service.root}\n"
                                    f"software_search = {discovery}\n")
        paths, queries_file = base / "paths", base / "queries"
        subprocess.run([args.fixture, str(size), str(paths), str(queries_file)], check=True)
        queries = [json.loads(line)["query"] for line in queries_file.read_text().splitlines()]
        service = Resident(args.daemon, base, size, environment=env)
        try:
            service.start()
            ready(service, software)
            service.stop()
            count = seed_catalog(service, paths.read_bytes().split(b"\0")[:-1])
            service.start()
            ready(service, software)
            workload(service, queries, software)
            pid = service.process.pid
            status, _ = service.call("status")
            before, start = cpu(pid), time.monotonic()
            time.sleep(16)  # cross one software stamp-check interval
            elapsed = time.monotonic() - start
            result = dict(entries=count, software=software, memory_kib=memory(pid),
                          cpu_percent_one_core=100 * (cpu(pid) - before) / elapsed,
                          cpu_includes="process and waited children", status=status)
            service.stop()
            if args.massif:
                service.command = ["valgrind", "--tool=massif", "--time-unit=B",
                                   "--detailed-freq=5", "--max-snapshots=60", "--threshold=1.0",
                                   f"--massif-out-file={args.output / (name + '.massif')}",
                                   *service.command]
                service.start()
                ready(service, software)
                workload(service, queries, software)
                service.stop()
            (args.output / (name + ".json")).write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result), flush=True)
        finally:
            service.close()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--daemon", default="build/release/torchlightd")
parser.add_argument("--fixture", default="build/bench_fixture")
parser.add_argument("--sizes", nargs="+", type=int, default=[50000, 500000])
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--massif", action="store_true")
args = parser.parse_args()
args.daemon = str(Path(args.daemon).resolve())
args.fixture = str(Path(args.fixture).resolve())
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
for size in args.sizes:
    for software in (False, True):
        profile(args, size, software)
