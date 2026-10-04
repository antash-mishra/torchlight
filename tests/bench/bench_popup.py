"""Measure native GTK paint latency while a 500k resident catalog is rebuilt."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from bench_daemon import Resident, percentiles, seed_catalog

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from popup_support import PopupDriver, wait_for

parser = argparse.ArgumentParser()
parser.add_argument("--launcher", default="build/torchlight-gtk")
parser.add_argument("--daemon", default="build/torchlightd-release")
parser.add_argument("--fixture", default="build/bench_fixture")
parser.add_argument("--xdotool", default="xdotool")
parser.add_argument("--size", type=int, default=500000)
parser.add_argument("--output")
args = parser.parse_args()
launcher, binary, fixture = [str(Path(value).resolve()) for value in
                             [args.launcher, args.daemon, args.fixture]]

with tempfile.TemporaryDirectory(prefix="torchlight-popup-bench-") as temporary:
    base = Path(temporary)
    live = base / "live"
    live.mkdir()
    data = base / "data"
    data.mkdir()
    for name in ["mime", "icons"]:
        (data / name).symlink_to("/usr/share/" + name, target_is_directory=True)
    for index in range(100):
        (live / f"initial-{index}.txt").touch()
    (base / "config").write_text(f"root = {base}/virtual\nroot = {live}\n")
    path_file, query_file = base / "paths", base / "queries"
    subprocess.run([fixture, str(args.size), str(path_file), str(query_file)], check=True)
    queries = [json.loads(line)["query"] for line in query_file.read_text().splitlines()]
    service = Resident(binary, base, args.size)
    gui = None
    with tempfile.TemporaryFile() as gui_log:
        try:
            service.start()
            service.idle()
            service.stop()
            entries = seed_catalog(service, path_file.read_bytes().split(b"\0")[:-1])
            service.start()
            service.idle()
            driver = PopupDriver(base, dict(os.environ, XDG_DATA_HOME=str(base / "data"),
                                           XDG_DATA_DIRS=str(base / "empty")), args.xdotool)
            for index in range(100):
                (live / f"update-{index}.txt").touch()
            marker = live / "arrival.txt"
            marker.touch()
            wait_for(lambda: service.call("status")[0]["indexing"]["active"], timeout=120)
            started = time.monotonic_ns() // 1000
            gui = subprocess.Popen([launcher, "--socket", str(service.socket_path), "--toggle"],
                                   env=driver.env, stdout=gui_log, stderr=gui_log)
            wait_for(driver.focused)
            first_focus = (time.monotonic_ns() // 1000 - started) / 1000
            first = driver.frame(lambda frame: frame["first"])
            first_paint = (first["paint_us"] - started) / 1000
            paint_during = []
            for query in queries:
                if not service.call("status")[0]["indexing"]["active"]:
                    break
                frame = driver.type_query(query, measure=False)
                # Count only frames bracketed by an active rebuild. A request
                # that straddles publication belongs to neither distribution.
                if service.call("status")[0]["indexing"]["active"]:
                    paint_during.append((frame["paint_us"] - frame["changed_us"]) / 1000)
            assert paint_during, "No painted results measured during the rebuild"
            service.idle()
            response, _ = service.call(query="arrival.txt", limit=10)
            assert any(row.get("path") == str(marker) for row in response["results"])
            for query in queries[:100]:
                driver.type_query(query)
            repeated_focus = []
            for _ in range(10):
                driver.xdo("key", "Escape")
                wait_for(lambda: not driver.focused())
                started = time.monotonic_ns() // 1000
                subprocess.run([launcher, "--toggle"], env=driver.env, check=True,
                               stdout=gui_log, stderr=gui_log, timeout=8)
                wait_for(driver.focused)
                repeated_focus.append((time.monotonic_ns() // 1000 - started) / 1000)
            output = dict(requested_paths=args.size, catalog_entries=entries,
                          display="private Xvfb/Metacity X11; native GTK after-paint",
                          daemon_build="C17 -O3 -DNDEBUG", launcher_build="C17 -O0 -g3",
                          first_focus_during_rebuild_ms=round(first_focus, 3),
                          first_paint_during_rebuild_ms=round(first_paint, 3),
                          repeat_focus_ms=percentiles(repeated_focus),
                          input_to_results_paint_ms=percentiles(driver.input_ms),
                          indexing_input_to_results_paint_ms=percentiles(paint_during))
            print(json.dumps(output, sort_keys=True), flush=True)
            if args.output:
                Path(args.output).write_text(json.dumps(output, indent=2) + "\n")
        finally:
            if gui is not None:
                gui.terminate()
                gui.wait(timeout=10)
                gui_log.seek(0)
                output = gui_log.read().decode(errors="replace")
                assert not any(word in output for word in ["CRITICAL", "ERROR", "Gtk-WARNING"]), output
            service.close()
