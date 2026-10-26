"""Merge the launcher comparison's per-box results into tables and chart data.

    report.py RESULTS_DIR OUT_PREFIX

RESULTS_DIR holds one folder per box (named after its launcher) with lscpu.txt and
the run_compare.py outputs <launcher>-round<N>.json. Writes OUT_PREFIX.md (tables)
and OUT_PREFIX.json (summary for the chart page), and fails when a launcher's
search quality differs between rounds or Torchlight's differs between boxes,
since quality must not depend on the machine.
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

SECTIONS = ("apps", "everyday", "files")
SECTION_TITLES = dict(apps="Apps, generated from every app's name", everyday="Everyday app queries",
                      files="Files in a 50k-path home")
KINDS = dict(apps=("name", "prefix", "typo"), everyday=("everyday",),
             files=("exact", "prefix", "abbreviation", "typo", "partial_typo", "parent"))
KIND_TITLES = dict(name="full name", prefix="first letters", typo="one typo", everyday="everyday",
                   exact="exact name", abbreviation="abbreviation", partial_typo="partial typo",
                   parent="folder + name")
ORDER = ("torchlight", "gnome", "ulauncher", "rofi", "fzf", "plocate")
TITLES = dict(torchlight="Torchlight", gnome="GNOME Shell search", ulauncher="Ulauncher", rofi="rofi",
              fzf="fzf", plocate="plocate")
TOP = 5


def load(results_dir):
    """{box: {launcher: [entry per round]}} plus each box's CPU model."""
    boxes, cpus = {}, {}
    for box in sorted(path for path in Path(results_dir).iterdir() if path.is_dir()):
        runs = {}
        for result in sorted(box.glob("*-round*.json")):
            for entry in json.loads(result.read_text())["results"]:
                runs.setdefault(entry["launcher"], []).append(entry)
        boxes[box.name] = runs
        cpus[box.name] = next((line.split(":", 1)[1].strip() for line in (box / "lscpu.txt").read_text().splitlines()
                               if line.startswith("Model name")), "?")
    return boxes, cpus


def ranks(entry, section):
    return [(record["query"], record["rank"]) for record in entry["sections"][section]]


def check_quality(boxes):
    problems = []
    reference = None
    for box, runs in boxes.items():
        for launcher, entries in runs.items():
            for section in entries[0]["sections"]:
                first = ranks(entries[0], section)
                if any(ranks(entry, section) != first for entry in entries[1:]):
                    problems.append(f"{launcher} on box {box}: {section} ranks differ between rounds")
        torchlight = runs["torchlight"][0]
        if reference is None:
            reference = (box, torchlight)
        elif any(ranks(torchlight, s) != ranks(reference[1], s) for s in SECTIONS):
            problems.append(f"Torchlight's ranks differ between boxes {reference[0]} and {box}")
    return problems


def quality(records):
    def share(test):
        return round(sum(test(record["rank"]) for record in records) / len(records), 4) if records else None
    return dict(n=len(records), first=share(lambda rank: rank == 1),
                top5=share(lambda rank: rank is not None and rank <= TOP))


def percentile(values, fraction):
    ordered = sorted(values)
    return round(ordered[min(len(ordered) - 1, int(len(ordered) * fraction))], 3)


def timing(entries, section):
    values = [record["ms"] for entry in entries for record in entry["sections"][section]]
    return dict(p50=percentile(values, 0.5), p95=percentile(values, 0.95))


def summarize(boxes, cpus):
    launchers = {}
    for box, runs in boxes.items():
        for launcher, entries in runs.items():
            if launcher == "torchlight":
                continue
            launchers[launcher] = dict(box=box, entries=entries, torchlight=runs["torchlight"])
    any_box = next(iter(boxes.values()))
    launchers["torchlight"] = dict(box=None, entries=any_box["torchlight"], torchlight=None)
    summary = {}
    for launcher in ORDER:
        if launcher not in launchers:
            continue
        info = launchers[launcher]
        entry = info["entries"][0]
        sections = {}
        for section, records in entry["sections"].items():
            sections[section] = dict(overall=quality(records),
                                     kinds={kind: quality([r for r in records if r["kind"] == kind])
                                            for kind in KINDS[section]},
                                     time_ms=timing(info["entries"], section))
            if section == "files":
                # Targets inside folders Torchlight skips by default (ADR 0062).
                kept = [r for r in records if not r.get("skipped_folder")]
                sections[section]["skipped_folders"] = len(records) - len(kept)
                sections[section]["without_skipped"] = quality(kept)
            if info["torchlight"]:
                sections[section]["torchlight_same_box_ms"] = timing(info["torchlight"], section)
        summary[launcher] = dict(title=TITLES[launcher], version=entry["version"], box=info["box"],
                                 cpu=cpus.get(info["box"]), sections=sections)
        if launcher == "torchlight":
            memory = [e["memory_kib"] for runs in boxes.values() for e in runs["torchlight"]]
            summary[launcher]["memory_mib"] = dict(
                rss=round(statistics.median(m["VmRSS"] for m in memory) / 1024, 1),
                peak=round(statistics.median(m["VmHWM"] for m in memory) / 1024, 1))
            summary[launcher]["start_s"] = statistics.median(
                e["start_s"] for runs in boxes.values() for e in runs["torchlight"])
    return summary


def percent(value):
    return "—" if value is None else f"{value * 100:.0f}%"


def quality_table(summary, section):
    kinds = KINDS[section]
    skipped = section == "files"
    header = ("| Launcher | Found first | Top 5 |" + (" Without skipped folders |" if skipped else "") +
              "".join(f" {KIND_TITLES[k]} |" for k in kinds if len(kinds) > 1))
    lines = [header, "|" + "---|" * (header.count("|") - 1)]
    for launcher, info in summary.items():
        if section not in info["sections"]:
            continue
        data = info["sections"][section]
        row = f"| {info['title']} | {percent(data['overall']['first'])} | {percent(data['overall']['top5'])} |"
        if skipped:
            kept = data["without_skipped"]
            row += f" {percent(kept['first'])} / {percent(kept['top5'])} |"
        if len(kinds) > 1:
            row += "".join(f" {percent(data['kinds'][k]['first'])} / {percent(data['kinds'][k]['top5'])} |"
                           for k in kinds)
        lines.append(row)
    n = next(info["sections"][section]["overall"]["n"] for info in summary.values() if section in info["sections"])
    note = (f"\n{n} queries. Per kind: found first / top 5." if len(kinds) > 1 else f"\n{n} queries.")
    if skipped:
        count = next(info["sections"]["files"]["skipped_folders"] for info in summary.values()
                     if "files" in info["sections"])
        note += (f" {count} of the targets sit in folders Torchlight skips by default (hidden"
                 " folders, build output, package folders; ADR 0062); the third column leaves"
                 " them out (found first / top 5).")
    return "\n".join(lines) + note


def time_table(summary):
    lines = ["| Launcher | Section | Its time p50 / p95 (ms) | Torchlight on the same box (ms) |",
             "|---|---|---|---|"]
    for launcher, info in summary.items():
        if launcher == "torchlight":
            continue
        for section, data in info["sections"].items():
            mine, theirs = data["time_ms"], data["torchlight_same_box_ms"]
            lines.append(f"| {info['title']} | {section} | {mine['p50']} / {mine['p95']} | "
                         f"{theirs['p50']} / {theirs['p95']} |")
    return "\n".join(lines)


def losses(boxes):
    """Queries some other launcher found first that Torchlight did not."""
    torchlight = {}
    for section in SECTIONS:
        for record in next(iter(boxes.values()))["torchlight"][0]["sections"][section]:
            torchlight[(section, record["query"])] = record
    lost = {}
    for runs in boxes.values():
        for launcher, entries in runs.items():
            if launcher == "torchlight":
                continue
            for section, records in entries[0]["sections"].items():
                for record in records:
                    ours = torchlight[(section, record["query"])]
                    if record["rank"] == 1 and ours["rank"] != 1:
                        item = lost.setdefault((section, record["query"]), dict(
                            section=section, kind=record["kind"], query=record["query"],
                            torchlight_rank=ours["rank"], torchlight_shown=ours["shown"], winners=[]))
                        item["winners"].append(launcher)
    return sorted(lost.values(), key=lambda item: (item["section"], item["kind"], item["query"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("results", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    boxes, cpus = load(args.results)
    problems = check_quality(boxes)
    summary = summarize(boxes, cpus)
    lost = losses(boxes)
    parts = [f"## {SECTION_TITLES[s]}\n\n{quality_table(summary, s)}" for s in SECTIONS]
    parts.append(f"## Time per query\n\n{time_table(summary)}")
    memory = summary["torchlight"]["memory_mib"]
    parts.append(f"Torchlight's daemon: {memory['rss']} MiB resident (peak {memory['peak']} MiB) with the "
                 f"89 apps and 50k paths; ready {summary['torchlight']['start_s']:.1f} s after starting.")
    parts.append("## Versions and boxes\n\n" + "\n".join(
        f"- {info['title']}: {info['version']}" + (f" (box {info['box']}, {info['cpu']})" if info["box"] else "")
        for info in summary.values()))
    args.out.with_suffix(".md").write_text("\n\n".join(parts) + "\n")
    args.out.with_suffix(".json").write_text(json.dumps(dict(summary=summary, losses=lost), indent=1) + "\n")
    print(args.out.with_suffix(".md").read_text())
    print(f"{len(lost)} queries another launcher found first and Torchlight did not")
    if problems:
        print("\n".join(problems), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
