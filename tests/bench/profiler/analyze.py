"""Symbolize sampler.c dumps: CPU per thread, top self and inclusive functions,
and folded stacks (one "thread;outer;...;leaf count" line each) for flame graphs.

Usage: analyze.py DUMP.prof [TOP]   (writes DUMP.folded next to the dump)

Symbols come from `nm`: a separate debug file under /usr/lib/debug/.build-id
when one is installed (libc), else the file's own symtab, else its dynsym.
Thread names are the symbolized start routines the sampler recorded."""
import bisect
import collections
import functools
import struct
import subprocess
import sys
from pathlib import Path

DEPTH = 40           # SAMPLER_DEPTH
PERIOD_MS = 2.0      # SAMPLER_PERIOD_NS
RECORD = struct.Struct("<II" + "Q" * DEPTH)
DEBUG_ROOT = Path("/usr/lib/debug/.build-id")
# A sample this far past the nearest symbol is in an unnamed region.
SYMBOL_REACH = 1 << 16


def debug_file(path):
    out = subprocess.run(["readelf", "-n", path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "Build ID:" in line:
            build = line.split(":", 1)[1].strip()
            candidate = DEBUG_ROOT / build[:2] / (build[2:] + ".debug")
            if candidate.exists():
                return str(candidate)
    return None


@functools.lru_cache(None)
def symbols(path):
    """Sorted function symbol addresses and names of one ELF file."""
    debug = debug_file(path)
    for command in (["nm", "-n", "--defined-only", debug or path],
                    ["nm", "-D", "-n", "--defined-only", path]):
        names = []
        out = subprocess.run(command, capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "TtWwiI":
                names.append((int(parts[0], 16), parts[2]))
        if names:
            names.sort()
            return [a for a, _ in names], [n for _, n in names]
    return [], []


@functools.lru_cache(None)
def load_bias(path):
    """Executable PT_LOAD vaddr - offset, mapping file offsets to symbol addresses."""
    out = subprocess.run(["readelf", "-lW", path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD" and "E" in parts[6:-1]:
            return int(parts[2], 16) - int(parts[1], 16)
    return 0


def parse(path):
    data = Path(path).read_bytes()
    position = 0

    def line():
        nonlocal position
        end = data.index(b"\n", position)
        text = data[position:end].decode(errors="replace")
        position = end + 1
        return text

    threads = {}
    for _ in range(int(line().split()[1])):
        tid, start = line().split()
        threads[int(tid)] = int(start, 16)
    assert line() == "MAPS"
    maps = []
    while (text := line()) != "END":
        parts = text.split(None, 5)
        low, high = (int(x, 16) for x in parts[0].split("-"))
        if "x" in parts[1] and len(parts) == 6 and parts[5].startswith("/"):
            maps.append((low, high, int(parts[2], 16), parts[5]))
    maps.sort()
    samples = []
    for i in range(int(line().split()[1])):
        record = RECORD.unpack_from(data, position + i * RECORD.size)
        samples.append((record[0], record[2:2 + record[1]]))
    return threads, maps, samples


class Symbolizer:
    def __init__(self, maps):
        self.maps = maps
        self.starts = [m[0] for m in maps]

    @functools.lru_cache(None)
    def name(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i < 0 or address >= self.maps[i][1]:
            return "[unknown]"
        low, _, offset, path = self.maps[i]
        local = address - low + offset + load_bias(path)
        addresses, names = symbols(path)
        j = bisect.bisect_right(addresses, local) - 1
        library = Path(path).name
        prefix = "" if library.startswith("torchlight") else library.split(".so")[0] + ":"
        if j < 0 or local - addresses[j] > SYMBOL_REACH:
            return f"{prefix}[{library}+{local:#x}]"
        return prefix + names[j]


def report(path, top=25, folded=None):
    threads, maps, samples = parse(path)
    symbolizer = Symbolizer(maps)
    thread_names = {tid: symbolizer.name(start) if start else "main" for tid, start in threads.items()}
    per_thread = collections.Counter()
    leaf = collections.defaultdict(collections.Counter)
    inclusive = collections.defaultdict(collections.Counter)
    stacks = collections.Counter()
    for tid, frames in samples:
        thread = thread_names.get(tid, f"tid{tid}")
        per_thread[thread] += 1
        # Return addresses point after the call; step back into it.
        names = [symbolizer.name(frames[0])] + [symbolizer.name(f - 1) for f in frames[1:]]
        for key in ("ALL", thread):
            leaf[key][names[0]] += 1
            for name in set(names):
                inclusive[key][name] += 1
        stacks[";".join([thread] + names[::-1])] += 1
    total = max(len(samples), 1)
    print(f"== {Path(path).name}: {len(samples)} samples = {len(samples) * PERIOD_MS / 1000:.2f} CPU-s")
    for thread, count in per_thread.most_common():
        print(f"   thread {thread:28s} {count * PERIOD_MS / 1000:8.2f} s  {100 * count / total:5.1f}%")
    for thread in ["ALL"] + [t for t, _ in per_thread.most_common(3)]:
        for title, counts in (("self (leaf)", leaf[thread]), ("inclusive", inclusive[thread])):
            print(f"-- [{thread}] {title}")
            for name, count in counts.most_common(top):
                print(f"   {100 * count / total:5.1f}%  {name}")
    if folded:
        with open(folded, "w") as out:
            for stack, count in stacks.items():
                out.write(f"{stack} {count}\n")


if __name__ == "__main__":
    dump = sys.argv[1]
    report(dump, int(sys.argv[2]) if len(sys.argv) > 2 else 25, str(Path(dump).with_suffix(".folded")))
