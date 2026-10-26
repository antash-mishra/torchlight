"""Write the launcher comparison as one self-contained HTML page.

    report_page.py BOAT_RESULTS DESKTOP_RESULTS LOSSES_JSON OUT.html

BOAT_RESULTS and DESKTOP_RESULTS are results folders as report.py reads them (one
folder per box with <launcher>-round<N>.json); LOSSES_JSON is report.py's JSON for
the Boat run. The numbers are embedded in report_page.html, which draws them with
plain SVG, so the page needs no network or server.
"""
import argparse
import json
import statistics
from pathlib import Path

TITLES = dict(torchlight="Torchlight", gnome="GNOME Shell search", ulauncher="Ulauncher", rofi="rofi",
              fzf="fzf", plocate="plocate")
SECTIONS = ("apps", "everyday", "files")
FILE_KINDS = ("exact", "prefix", "abbreviation", "typo", "partial_typo", "parent")
APP_KINDS = ("name", "prefix", "typo")
TOP = 5
TEMPLATE = Path(__file__).with_name("report_page.html")


def load(root):
    """{launcher: [entry per round and box]}, the CPU models seen and the corpus sizes."""
    runs, cpus, corpus = {}, set(), {}
    for box in sorted(path for path in Path(root).iterdir() if path.is_dir()):
        for line in (box / "lscpu.txt").read_text().splitlines():
            if line.startswith("Model name"):
                cpus.add(line.split(":", 1)[1].strip())
        for result in sorted(box.glob("*-round*.json")):
            content = json.loads(result.read_text())
            corpus = content["corpus"]
            for entry in content["results"]:
                runs.setdefault(entry["launcher"], []).append(entry)
    return runs, sorted(cpus), corpus


def share(records, test):
    return round(sum(test(record["rank"]) for record in records) / len(records), 4)


def found_first(records):
    return share(records, lambda rank: rank == 1)


def top_five(records):
    return share(records, lambda rank: rank is not None and rank <= TOP)


def percentile(values, fraction):
    ordered = sorted(values)
    return round(ordered[min(len(ordered) - 1, int(len(ordered) * fraction))], 4)


def quality(boat, desktop):
    rows = []
    for launcher, title in TITLES.items():
        for section in SECTIONS:
            if section not in boat[launcher][0]["sections"]:
                continue
            cloud = boat[launcher][0]["sections"][section]
            local = desktop[launcher][0]["sections"][section]
            rows.append(dict(launcher=title, section=section, queries=len(cloud), found_first=found_first(cloud),
                             top5=top_five(cloud), found_first_desktop=found_first(local),
                             top5_desktop=top_five(local)))
    return rows


def kinds(runs, section, names):
    rows = []
    for launcher, title in TITLES.items():
        if section not in runs[launcher][0]["sections"]:
            continue
        records = runs[launcher][0]["sections"][section]
        row = dict(launcher=title)
        for name in names:
            row[name] = found_first([record for record in records if record["kind"] == name])
        rows.append(row)
    return rows


def timing(machines):
    rows = []
    for launcher, title in TITLES.items():
        for machine, runs in machines:
            for section in ("apps", "files"):
                if section not in runs[launcher][0]["sections"]:
                    continue
                times = [record["ms"] for entry in runs[launcher] for record in entry["sections"][section]]
                rows.append(dict(launcher=title, machine=machine, section=section, queries=len(times),
                                 p50_ms=percentile(times, 0.5), p95_ms=percentile(times, 0.95)))
    return rows


def daemon(machines):
    rows = []
    for machine, runs in machines:
        entries = runs["torchlight"]
        rows.append(dict(machine=machine,
                         rss_mib=round(statistics.median(e["memory_kib"]["VmRSS"] for e in entries) / 1024, 1),
                         peak_mib=round(statistics.median(e["memory_kib"]["VmHWM"] for e in entries) / 1024, 1),
                         ready_s=round(statistics.median(e["start_s"] for e in entries), 2)))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("boat", type=Path)
    parser.add_argument("desktop", type=Path)
    parser.add_argument("losses", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    (boat, boat_cpus, corpus), (desktop, desktop_cpus, _) = load(args.boat), load(args.desktop)
    machines = (("Boat", boat), ("desktop", desktop))
    losses = [dict(section=item["section"], kind=item["kind"], query=item["query"],
                   torchlight_rank=item["torchlight_rank"], won_by=[TITLES[w] for w in item["winners"]])
              for item in json.loads(args.losses.read_text())["losses"]]
    data = dict(
        version=boat["torchlight"][0]["version"], boat_cpus=boat_cpus, desktop_cpus=desktop_cpus,
        apps=corpus["apps"], paths=corpus["paths"],
        quality=quality(boat, desktop), file_kinds=kinds(boat, "files", FILE_KINDS),
        app_kinds=kinds(boat, "apps", APP_KINDS), timing=timing(machines), daemon=daemon(machines),
        losses=losses)
    # "</" would end the script element the data sits in.
    embedded = json.dumps(data, separators=(",", ":")).replace("</", "<\\/")
    page = TEMPLATE.read_text().replace("/*DATA*/null", embedded)
    args.out.write_text(page)
    print(f"wrote {args.out} ({len(page) // 1024} KiB)")


if __name__ == "__main__":
    main()
