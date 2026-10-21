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


class Text(C.Structure):
    _fields_ = [("symbols", C.c_void_p), ("boundaries", C.c_void_p), ("byte_offsets", C.c_void_p),
                ("length", C.c_size_t), ("basename", C.c_size_t), ("mask", C.c_uint64)]


# lexical_exactness's tiers for exact names and raw paths, prefix_score's initials
# score (the weakest evidence that a query word starts an application's name) and
# token score (the weakest that it starts a word of a generic name or keywords).
LEXICAL_EXACT_NAME = 1
LEXICAL_EXACT_RAW_PATH = 2
PREFIX_INITIALS_SCORE = 4500
PREFIX_TOKEN_SCORE = 5000
# desktop_evidence's classes (ADR 0061).
FUZZY, WORD_START, DESCRIBED, NAMED = range(4)


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
        # Libraries that can say which applications a query names (ADR 0058) are
        # merged as the daemon does now; older ones by score, as it did then.
        self.naming = hasattr(self.lib, "desktop_named")
        if self.naming:
            signatures.update({
                "tokenize_create": ([C.c_char_p, C.POINTER(C.c_void_p)], C.c_int),
                "tokenize_view": ([C.c_void_p], Text),
                "tokenize_destroy": ([C.c_void_p], None),
                "prefix_score": ([Text, Text, C.POINTER(C.c_int), C.POINTER(C.c_bool)], C.c_int),
                "lexical_exactness": ([C.POINTER(Result)], C.c_int),
            })
        # Libraries from before M8 have no place ranking; file engines use it when present.
        self.place = getattr(self.lib, "lexical_set_place_ranking", None)
        # Libraries with word-start evidence merge as the daemon does since ADR 0061.
        self.word_starts = self.naming and hasattr(self.lib, "lexical_word_start")
        if self.word_starts:
            signatures["lexical_word_start"] = ([C.POINTER(Result)], C.c_bool)
        self.metadata = {number: (entry.get("generic", ""), entry.get("keywords", ""))
                         for number, entry in enumerate(entries, 1) if entry["kind"] == "app"}
        if self.place:
            signatures["lexical_set_place_ranking"] = ([C.c_void_p, C.c_bool], C.c_int)
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
                if self.place and not desktop:
                    self.check(self.place(engine, True))
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

    def tokenized(self, text):
        handle = C.c_void_p()
        self.check(self.lib.tokenize_create(text.encode("utf-8", "surrogateescape"),
                                            C.byref(handle)))
        return handle

    def prefix(self, text, word):
        """prefix_score of word against text: its best score and whether a whole
        token matched; (0, False) for text without tokens."""
        if not text.strip():
            return 0, False
        handles = [self.tokenized(text), self.tokenized(word)]
        try:
            best, complete = C.c_int(), C.c_bool()
            self.check(self.lib.prefix_score(self.lib.tokenize_view(handles[0]),
                                             self.lib.tokenize_view(handles[1]),
                                             C.byref(best), C.byref(complete)))
            return best.value, complete.value
        finally:
            for handle in handles:
                self.lib.tokenize_destroy(handle)

    def names(self, query, path):
        """Whether every word of query starts the application's name, a word of it
        or its initials, as desktop_named decides."""
        words = [word for word in re.split(r"[ \t\n\r]+", query) if word]
        name = path.rsplit("/", 1)[1]
        return bool(words) and all(self.prefix(name, word)[0] >= PREFIX_INITIALS_SCORE
                                   for word in words)

    def evidence(self, query, number, path):
        """What query's words find in application number, as desktop_evidence
        decides; its generic field also holds the name, as the catalog stores it."""
        words = [word for word in re.split(r"[ \t\n\r]+", query) if word]
        name = path.rsplit("/", 1)[1]
        generic, keywords = self.metadata[number]
        named = whole = True
        for word in words:
            if self.prefix(name, word)[0] >= PREFIX_INITIALS_SCORE:
                continue
            named = False
            matches = [self.prefix(field, word) for field in (name + " " + generic, keywords)]
            found = [complete for best, complete in matches if best >= PREFIX_TOKEN_SCORE]
            if not found:
                return FUZZY
            whole = whole and any(found)
        if not words:
            return FUZZY
        return NAMED if named else DESCRIBED if whole else WORD_START

    def merge(self, text, files, apps, key):
        """The daemon's merge since ADR 0061: a one-word query keeps only
        word-start matches once any exists, then exact raw paths, named
        applications, exact names, described applications and the rest by score."""
        evidence = {row.id: self.evidence(text, row.id, row.path.decode()) for row in apps}
        words = [word for word in re.split(r"[ \t\n\r]+", text) if word]
        if len(words) == 1 and "/" not in text:
            starts = [row for row in files if self.lib.lexical_word_start(C.byref(row))]
            if starts or any(evidence[row.id] != FUZZY for row in apps):
                files = starts
                apps = [row for row in apps if evidence[row.id] != FUZZY]
        exactness = lambda row: self.lib.lexical_exactness(C.byref(row))  # noqa: E731
        exact = [row for row in files if exactness(row) == LEXICAL_EXACT_RAW_PATH]
        named = [row for row in apps if evidence[row.id] == NAMED]
        names = [row for row in files if exactness(row) == LEXICAL_EXACT_NAME]
        described = [row for row in apps if evidence[row.id] == DESCRIBED]
        placed = exact + named + names + described
        rest = [row for row in files + apps if row not in placed]
        return placed + sorted(rest, key=key)

    def search(self, engine, workspace, text, capacity, cold):
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
        return rows[:count.value]

    def query(self, text, capacity=1000, cold=False):
        (files_engine, files_workspace), (apps_engine, apps_workspace) = self.engines
        files = self.search(files_engine, files_workspace, text, capacity, cold)
        # The daemon weighs every application match, not just capacity of them.
        apps = self.search(apps_engine, apps_workspace, text, 1000, cold)
        key = lambda row: (-row.score, row.path, row.id)  # noqa: E731
        if self.word_starts:
            merged = self.merge(text, files, apps, key)
        elif not self.naming:
            merged = sorted(files + apps, key=key)
        else:
            exact = [row for row in files
                     if self.lib.lexical_exactness(C.byref(row)) == LEXICAL_EXACT_RAW_PATH]
            named = [row for row in apps if self.names(text, row.path.decode())]
            rest = [row for row in files + apps if row not in exact and row not in named]
            merged = exact + named + sorted(rest, key=key)
        return [self.ids[row.id] for row in merged[:capacity]]

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
