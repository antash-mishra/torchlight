#!/usr/bin/env python3
"""Evaluate native mixed-catalog relevance against frozen and BM25 baselines.

Target groups stay together in the explicit tuning/held-out split. Fixture size
is below native result capacity, so recall measures complete returned membership.
BM25 is an exact-token field baseline, without prefix or edit expansion.
"""
import argparse
import collections
import ctypes as C
import json
import math
import pathlib
import re
import unicodedata


class Result(C.Structure):
    _fields_ = [("id", C.c_uint64), ("path", C.c_char_p), ("score", C.c_int)]


class Engine:
    def __init__(self, library, entries):
        self.lib = C.CDLL(str(pathlib.Path(library).resolve()))
        signatures = {
            "lexical_create": ([C.POINTER(C.c_void_p)], C.c_int),
            "lexical_set_prefix_bonus": ([C.c_void_p, C.c_int], C.c_int),
            "lexical_add": ([C.c_void_p, C.c_uint64, C.c_char_p, C.c_bool], C.c_int),
            "lexical_finish": ([C.c_void_p], C.c_int),
            "lexical_workspace_create": ([C.c_void_p, C.POINTER(C.c_void_p)], C.c_int),
            "lexical_query": ([C.c_void_p, C.c_void_p, C.c_char_p, C.POINTER(Result),
                               C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
            "lexical_workspace_destroy": ([C.c_void_p], None),
            "lexical_destroy": ([C.c_void_p], None),
        }
        self.fields = getattr(self.lib, "lexical_add_fields", None)
        if self.fields:
            signatures["lexical_add_fields"] = (
                [C.c_void_p, C.c_uint64, C.c_char_p, C.c_char_p, C.c_char_p], C.c_int)
        for name, (arguments, result) in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes, function.restype = arguments, result
        self.engines = []
        self.ids = {}
        try:
            for desktop in (False, True):
                engine, workspace = C.c_void_p(), C.c_void_p()
                self.check(self.lib.lexical_create(C.byref(engine)))
                self.engines.append([engine, workspace])
                self.check(self.lib.lexical_set_prefix_bonus(engine, 2000 if desktop else 0))
                for number, entry in enumerate(entries, 1):
                    if (entry["kind"] == "app") != desktop:
                        continue
                    self.ids[number] = entry["key"]
                    path = entry["path"].encode("utf-8", "surrogateescape")
                    if desktop and self.fields:
                        status = self.fields(engine, number, path, entry.get("generic", "").encode(),
                                             entry.get("keywords", "").encode())
                    else:
                        if desktop:
                            metadata = " ".join((entry["path"].rsplit("/", 1)[1],
                                                 entry.get("generic", ""), entry.get("keywords", "")))
                            path = ("/Applications/" + metadata.replace("/", " ") + "/" +
                                    entry["path"].rsplit("/", 1)[1]).encode()
                        status = self.lib.lexical_add(engine, number, path, False)
                    self.check(status)
                self.check(self.lib.lexical_finish(engine))
                self.check(self.lib.lexical_workspace_create(engine, C.byref(workspace)))
        except BaseException:
            self.close()
            raise

    @staticmethod
    def check(status):
        if status:
            raise RuntimeError(f"native engine returned status {status}")

    def query(self, text, capacity=1000, cold=False):
        merged = []
        for engine, workspace in self.engines:
            rows, count = (Result * capacity)(), C.c_size_t()
            if cold:
                workspace = C.c_void_p()
                self.check(self.lib.lexical_workspace_create(engine, C.byref(workspace)))
            try:
                self.check(self.lib.lexical_query(engine, workspace, text.encode(), rows,
                                                 capacity, C.byref(count)))
            finally:
                if cold:
                    self.lib.lexical_workspace_destroy(workspace)
            merged.extend((row.score, row.path, row.id) for row in rows[:count.value])
        merged.sort(key=lambda row: (-row[0], row[1], row[2]))
        return [self.ids[row[2]] for row in merged[:capacity]]

    def close(self):
        for engine, workspace in self.engines:
            self.lib.lexical_workspace_destroy(workspace)
            self.lib.lexical_destroy(engine)
        self.engines = []


def tokens(text):
    text = re.sub(r"([a-z])([A-Z])", r"\1 \2", text)
    return re.findall(r"\w+", unicodedata.normalize("NFC", text.casefold()))


class BM25:
    def __init__(self, entries):
        self.keys = [entry["key"] for entry in entries]
        self.documents = []
        for entry in entries:
            parent, name = entry["path"].rsplit("/", 1)
            self.documents.append([tokens(name), tokens(entry.get("generic", "")),
                                   tokens(entry.get("keywords", "")), tokens(parent)])
        self.averages = [sum(len(doc[f]) for doc in self.documents) / len(entries)
                         for f in range(4)]
        self.frequency = collections.Counter(word for doc in self.documents
                                             for word in set(sum(doc, [])))

    def query(self, query):
        scored = []
        for key, document in zip(self.keys, self.documents):
            score = 0
            for word in tokens(query):
                frequency = self.frequency[word]
                idf = math.log(1 + (len(self.keys) - frequency + .5) / (frequency + .5))
                for field, weight in enumerate((3, 1.5, 1, .5)):
                    count = document[field].count(word)
                    norm = 1.2 * (.25 + .75 * len(document[field]) /
                                  (self.averages[field] or 1))
                    score += weight * idf * count * 2.2 / (count + norm)
            if score:
                scored.append((score, key))
        return [key for _, key in sorted(scored, key=lambda row: (-row[0], row[1]))]


def metrics(results, labels):
    if not labels:
        return {"empty_correct": float(not results)}
    ranks = [rank for rank, key in enumerate(results, 1) if labels.get(key, 0)]
    first = min(ranks, default=1001)
    dcg = sum((2 ** labels.get(key, 0) - 1) / math.log2(rank + 1)
              for rank, key in enumerate(results[:10], 1))
    ideal = sum((2 ** grade - 1) / math.log2(rank + 1)
                for rank, grade in enumerate(sorted(labels.values(), reverse=True)[:10], 1))
    return {"candidate_recall": len(ranks) / len(labels), "first_useful_rank": first,
            "mrr": 1 / first if ranks else 0, "top10_success": float(first <= 10),
            "ndcg10": dcg / ideal}


def evaluate(search, queries):
    buckets = collections.defaultdict(list)
    rows = []
    for query in queries:
        results = search.query(query["text"])
        values = metrics(results, query["labels"])
        rows.append({**query, "metrics": values, "top10": results[:10]})
        for kind in (query["class"], "all"):
            buckets[query["split"] + "/" + kind].append(values)
        if isinstance(search, Engine):
            assert search.query(query["text"], 3) == results[:3], query
            assert search.query(query["text"], cold=True) == results, query
    aggregate = {}
    for group, values in sorted(buckets.items()):
        measures = set().union(*(value.keys() for value in values))
        aggregate[group] = {"queries": len(values), **{
            key: sum(value[key] for value in values if key in value) /
                 sum(key in value for value in values) for key in sorted(measures)}}
    return {"aggregate": aggregate, "queries": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--current", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    fixture = json.loads(pathlib.Path(__file__).with_name("mixed.json").read_text())
    entries = fixture["entries"]
    for entry in entries:
        if "path_hex" in entry:
            entry["path"] = bytes.fromhex(entry["path_hex"]).decode("utf-8", "surrogateescape")
    # Variant families must never leak across the tuning/held-out boundary.
    groups = collections.defaultdict(set)
    for query in fixture["queries"]:
        groups[query["target_group"]].add(query["split"])
    assert all(len(splits) == 1 for splits in groups.values())
    report = {"fixture": "mixed.json", "candidate_capacity": 1000,
              "baseline_library": args.baseline, "current_library": args.current,
              "baseline_revision": "7ace90f0a3d1474a0594b08ee8db9fde72e0f713",
              "fixture_entries": len(entries), "fixture_queries": len(fixture["queries"])}
    for name, library in (("baseline", args.baseline), ("current", args.current)):
        engine = Engine(library, entries)
        try:
            report[name] = evaluate(engine, fixture["queries"])
        finally:
            engine.close()
    report["bm25"] = evaluate(BM25(entries), fixture["queries"])
    pathlib.Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    before = report["baseline"]["aggregate"]["held_out/all"]
    after = report["current"]["aggregate"]["held_out/all"]
    print(json.dumps({name: report[name]["aggregate"] for name in ("baseline", "current", "bm25")},
                     indent=2))
    assert after["ndcg10"] > before["ndcg10"], "held-out graded relevance must improve"
    assert after["candidate_recall"] > before["candidate_recall"], "held-out recall must improve"


if __name__ == "__main__":
    main()
