"""Native first-paint and PSS measurements over 100 warm show/query/hide cycles.

Run through tests/run_popup_checks.py --memory on a private display and bus.
The same workload can compare old and new launchers with --launcher.
Warm the complete 100-query workload before the baseline so first-use font,
shader and text caches are not mistaken for growth on repeated use.
The default hidden interval lets the 500 ms allocator-release policy run;
--hidden-ms 300 separately stresses rapid reopening that cancels that policy.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from bench_daemon import Resident, percentiles

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from popup_support import PopupDriver, wait_for


def memory(pid):
    keys = {"Rss", "Pss", "Pss_Anon", "Private_Dirty", "Swap"}
    return {line.split(":")[0]: int(line.split()[1])
            for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
            if line.split(":")[0] in keys}


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--launcher", default="build/release/torchlight-gtk")
parser.add_argument("--daemon", default="build/release/torchlightd")
parser.add_argument("--xdotool", default="xdotool")
parser.add_argument("--hidden-ms", type=int, default=1000)
parser.add_argument("--output")
args = parser.parse_args()
if args.hidden_ms < 0:
    parser.error("--hidden-ms must be nonnegative")
launcher, daemon = (str(Path(value).resolve()) for value in (args.launcher, args.daemon))

with tempfile.TemporaryDirectory(prefix="torchlight-popup-memory-") as temporary:
    base = Path(temporary)
    root = base / "files"
    root.mkdir()
    for index in range(100):
        (root / f"sample-{index:03}.txt").write_text("preview fixture\n")
    (base / "config").write_text(f"root = {root}\nsoftware_search = false\nweb_search = false\n")
    data = base / "data"
    data.mkdir()
    for name in ("icons", "mime"):
        (data / name).symlink_to("/usr/share/" + name, target_is_directory=True)
    driver = PopupDriver(base, dict(os.environ, XDG_DATA_HOME=str(data),
                                   XDG_DATA_DIRS=str(base / "empty")), args.xdotool)
    service = Resident(daemon, base, 100)
    gui = None
    with tempfile.TemporaryFile() as log:
        try:
            service.start()
            service.idle()
            start = time.monotonic_ns() // 1000
            gui = subprocess.Popen([launcher, "--socket", str(service.socket_path), "--toggle"],
                                   env=driver.env, stdout=log, stderr=log)
            first = driver.frame(lambda frame: frame["first"], after=start)
            cold_ms = (first["paint_us"] - start) / 1000
            wait_for(driver.focused)
            warm_paint = []
            samples = []
            warmup, measured = 100, 100
            checkpoints = (warmup - 1, warmup + measured - 1)
            for cycle in range(warmup + measured):
                driver.type_query(f"sample-{cycle % 100:03}", measure=True)
                if cycle in checkpoints:
                    time.sleep(.5)
                    shown = memory(gui.pid)
                driver.xdo("key", "Escape")
                wait_for(lambda: not driver.focused())
                time.sleep(args.hidden_ms / 1000)
                assert gui.poll() is None
                if cycle in checkpoints:
                    time.sleep(max(0, 1 - args.hidden_ms / 1000))
                    samples.append(dict(cycle=cycle, shown=shown, hidden=memory(gui.pid)))
                start = time.monotonic_ns() // 1000
                subprocess.run([launcher, "--toggle"], env=driver.env, stdout=log,
                               stderr=log, check=True, timeout=8)
                frame = driver.frame(lambda item: item["query"] == "", after=start)
                wait_for(driver.focused)
                if cycle >= warmup:
                    warm_paint.append((frame["paint_us"] - start) / 1000)
            result = dict(renderer=os.environ.get("GSK_RENDERER"), hidden_ms=args.hidden_ms,
                          warmup_cycles=warmup,
                          cycles=measured, units="KiB",
                          cold_paint_ms=cold_ms, warm_paint_ms=percentiles(warm_paint),
                          query_paint_ms=percentiles(driver.input_ms), samples=samples,
                          daemon=memory(service.process.pid))
            result["hidden_pss_growth_mib"] = (samples[-1]["hidden"]["Pss"] -
                                               samples[0]["hidden"]["Pss"]) / 1024
            print(json.dumps(result, indent=2))
            if args.output:
                Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
            assert result["hidden_pss_growth_mib"] <= 5, result
        finally:
            if gui is not None and gui.poll() is None:
                gui.terminate()
                gui.wait(timeout=10)
            service.close()
            log.seek(0)
            errors = log.read().decode(errors="replace")
            assert "Gtk-CRITICAL" not in errors and "GLib-CRITICAL" not in errors, errors
