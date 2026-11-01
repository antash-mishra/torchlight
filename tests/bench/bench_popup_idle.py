"""Compare fresh-process paint, warm reopening and RAM after the real 60-second timeout.

Run via run_popup_checks.py --idle-bench inside Boat. --baseline expects the old
resident build; both builds receive the same workload and 65-second hidden wait.
Fresh process timings use a warm OS file cache, not a cold boot.
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


def memory(process):
    if process.poll() is not None:
        return dict(Rss=0, Pss=0, Pss_Anon=0, Private_Dirty=0)
    keys = {"Rss", "Pss", "Pss_Anon", "Private_Dirty"}
    return {line.split(":")[0]: int(line.split()[1])
            for line in Path(f"/proc/{process.pid}/smaps_rollup").read_text().splitlines()
            if line.split(":")[0] in keys}


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--launcher", required=True)
parser.add_argument("--daemon", default="build/release/torchlightd")
parser.add_argument("--xdotool", default="xdotool")
parser.add_argument("--baseline", action="store_true")
parser.add_argument("--output", required=True)
args = parser.parse_args()
launcher, daemon = (str(Path(p).resolve()) for p in [args.launcher, args.daemon])

with tempfile.TemporaryDirectory(prefix="torchlight-idle-bench-") as temporary:
    base = Path(temporary)
    root = base / "files"
    root.mkdir()
    for i in range(100):
        (root / f"sample-{i:03}.txt").write_text("preview fixture\n")
    (base / "config").write_text(f"root = {root}\nsoftware_search = false\nweb_search = false\n")
    data = base / "data"
    data.mkdir()
    for name in ["icons", "mime"]:
        (data / name).symlink_to("/usr/share/" + name, target_is_directory=True)
    driver = PopupDriver(base, dict(os.environ, XDG_DATA_HOME=str(data),
                                   XDG_DATA_DIRS=str(base / "empty")), args.xdotool)
    service = Resident(daemon, base, 100)
    gui = None
    fresh, warm, typed = [], [], []
    with tempfile.TemporaryFile() as log:
        def show(new_process):
            started = time.monotonic_ns() // 1000
            command = [launcher, "--socket", str(service.socket_path), "--toggle"]
            if new_process:
                process = subprocess.Popen(command, env=driver.env, stdout=log, stderr=log)
            else:
                subprocess.run(command, env=driver.env, stdout=log, stderr=log,
                               check=True, timeout=8)
                process = gui
            frame = driver.frame(lambda f: f["query"] == "", after=started)
            wait_for(driver.focused)
            return process, (frame["paint_us"] - started) / 1000

        try:
            service.start()
            service.idle()
            for i in range(20):
                gui, elapsed = show(True)
                fresh.append(elapsed)
                driver.type_query(f"sample-{i:03}")
                if i != 19:
                    gui.terminate()
                    gui.wait(timeout=10)
            for i in range(20):
                driver.xdo("key", "Escape")
                wait_for(lambda: not driver.focused())
                time.sleep(1)
                gui, elapsed = show(False)
                warm.append(elapsed)
                driver.type_query(f"sample-{i:03}")
                typed.append(driver.input_ms[-1])
            shown = memory(gui)
            driver.xdo("key", "Escape")
            wait_for(lambda: not driver.focused())
            hidden_at = time.monotonic()
            time.sleep(1)
            hidden_warm = memory(gui)
            exited_after = None
            while time.monotonic() - hidden_at < 65:
                if exited_after is None and gui.poll() is not None:
                    assert gui.returncode == 0
                    exited_after = time.monotonic() - hidden_at
                time.sleep(.1)
            hidden_idle = memory(gui)
            assert (gui.poll() is None) == args.baseline
            service_memory = memory(service.process)
            gui, after_idle = show(not args.baseline)
            ready = driver.type_query("sample-019")
            assert ready["rows"] > 0
            result = dict(baseline=args.baseline, renderer=os.environ.get("GSK_RENDERER"),
                          units="KiB", fresh_process_paint_ms=percentiles(fresh),
                          warm_reopen_paint_ms=percentiles(warm),
                          warm_query_paint_ms=percentiles(typed),
                          reopen_after_idle_paint_ms=after_idle,
                          idle_exit_seconds=exited_after, shown=shown,
                          hidden_after_one_second=hidden_warm,
                          hidden_after_65_seconds=hidden_idle, daemon=service_memory,
                          combined_hidden_pss_mib=(hidden_idle["Pss"] + service_memory["Pss"])/1024)
            Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2), flush=True)
        finally:
            if gui is not None and gui.poll() is None:
                gui.terminate()
                gui.wait(timeout=10)
            service.close()
            log.seek(0)
            errors = log.read().decode(errors="replace")
            assert "Gtk-CRITICAL" not in errors and "GLib-CRITICAL" not in errors, errors
