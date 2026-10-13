#!/usr/bin/env python3
"""Compare file rankings of two engine libraries on a real path list (M8).

Loads each library (built with `make build/quality_engine.so`, or from an
older revision), indexes a NUL-separated absolute path list (BENCH_PATHS
format; the common directory is the indexed root) as the daemon's file
catalog does (place ranking on when the library has it), runs the queries and
counts, per query, top-10 results inside third-party trees and inside the
person's own folders. Both kinds are given as path substrings relative to the
root, so the counts are labels independent of the engine's own rules.

  place_eval.py --paths FILE --library OLD.so --library NEW.so \\
      --third-party /Android/Sdk/ --own /workspace/ [--show sc] [--output out.json]
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path

QUERIES = ("sc,scripts,readme,makefile,main,src,plan,torch,config,test,notes,doc,bench,report,"
           "todo,index,docker,png,setup,app,util,engine,shader,model,vaani,invaders,cmake,build,"
           "license,api,rank,lexical,crawl,writer,catalog,store,daemon,popup,frizbee,invoice,"
           "resume,photo,screenshot,video,download,keystore,gradle,manifest,kotlin,android")
TOP = 10


class Result(C.Structure):
    _fields_ = [("id", C.c_uint64), ("path", C.c_char_p), ("score", C.c_int)]


def common_directory(paths):
    prefix = os.path.commonpath(paths)
    while not all(p == prefix or p.startswith(prefix + b"/") for p in paths):
        prefix = prefix.rsplit(b"/", 1)[0] or b"/"
    return prefix


class Engine:
    def __init__(self, library, paths, root):
        self.lib = C.CDLL(str(Path(library).resolve()))
        self.lib.lexical_create.argtypes = [C.POINTER(C.c_void_p)]
        self.lib.lexical_add.argtypes = [C.c_void_p, C.c_uint64, C.c_char_p, C.c_bool]
        self.lib.lexical_finish.argtypes = [C.c_void_p]
        self.lib.lexical_workspace_create.argtypes = [C.c_void_p, C.POINTER(C.c_void_p)]
        self.lib.lexical_query.argtypes = [C.c_void_p, C.c_void_p, C.c_char_p, C.POINTER(Result),
                                           C.c_size_t, C.POINTER(C.c_size_t)]
        self.engine, self.workspace = C.c_void_p(), C.c_void_p()
        self.check(self.lib.lexical_create(C.byref(self.engine)))
        self.place = hasattr(self.lib, "lexical_set_place_ranking")
        if self.place:
            self.lib.lexical_set_place_ranking.argtypes = [C.c_void_p, C.c_bool]
            self.check(self.lib.lexical_set_place_ranking(self.engine, True))
        for number, path in enumerate(paths, 1):
            self.check(self.lib.lexical_add(self.engine, number, path, path == root))
        self.check(self.lib.lexical_finish(self.engine))
        self.check(self.lib.lexical_workspace_create(self.engine, C.byref(self.workspace)))

    @staticmethod
    def check(status):
        if status:
            raise RuntimeError(f"native engine returned status {status}")

    def query(self, text):
        rows, count = (Result * TOP)(), C.c_size_t()
        self.check(self.lib.lexical_query(self.engine, self.workspace, text.encode(), rows, TOP,
                                         C.byref(count)))
        return [rows[i].path for i in range(count.value)]


def classify(relative, third_party, own):
    if any(marker in relative for marker in third_party):
        return "third_party"
    return "own" if any(relative.startswith(marker) for marker in own) else "other"


def evaluate(engine, queries, root, third_party, own, show):
    totals = {"third_party": 0, "own": 0, "other": 0}
    per_query, shown = {}, {}
    for query in queries:
        relative = [path[len(root):] or b"/" for path in engine.query(query)]
        kinds = [classify(path, third_party, own) for path in relative]
        per_query[query] = {kind: kinds.count(kind) for kind in totals}
        for kind in kinds:
            totals[kind] += 1
        if query in show:
            shown[query] = [os.fsdecode(path) for path in relative]
    return {"place_ranking": engine.place, "totals": totals, "queries": per_query, "shown": shown}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--paths", required=True, type=Path)
    parser.add_argument("--library", action="append", required=True)
    parser.add_argument("--third-party", action="append", default=[])
    parser.add_argument("--own", action="append", default=[])
    parser.add_argument("--queries", default=QUERIES)
    parser.add_argument("--show", default="")
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    paths = sorted({p for p in options.paths.read_bytes().split(b"\0") if p.startswith(b"/")})
    root = common_directory(paths)
    queries = [q for q in options.queries.split(",") if q]
    third_party = [os.fsencode(m) for m in options.third_party]
    own = [os.fsencode(m) for m in options.own]
    show = set(options.show.split(","))
    report = {"entries": len(paths), "queries": len(queries), "top": TOP, "libraries": {}}
    for library in options.library:
        engine = Engine(library, paths, root)
        report["libraries"][library] = evaluate(engine, queries, root, third_party, own, show)
    for library, result in report["libraries"].items():
        print(f"{library}: place={result['place_ranking']} {json.dumps(result['totals'])}")
    if options.output:
        options.output.write_text(json.dumps(report, indent=1) + "\n")


if __name__ == "__main__":
    main()
