#!/usr/bin/env python3
"""Reproduce local Model2Vec retrieval and held-out launcher fusion evaluation.

Install model2vec in an isolated environment. Model files are local; this script
never downloads models or sends queries to a service. Tune on tuning rows only.
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import resource
import time

import numpy as np
from model2vec import StaticModel

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("quality", ROOT / "tests/quality/evaluate.py")
quality = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quality)


def prepare(entry):
    """Filename stem, extension, two nearby parents; app name and metadata."""
    path = entry["path"]
    if entry["kind"] == "app":
        return " ".join((path.rsplit("/", 1)[1], entry.get("generic", ""),
                         entry.get("keywords", "").replace(";", " ")))
    return " ".join(path.rsplit("/", 3)[-3:]).replace("_", " ").replace("-", " ").replace(".", " ")


def normalized(values):
    return values / np.maximum(np.linalg.norm(values, axis=1, keepdims=True), 1e-32)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--library", default="build/quality_engine.so")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    fixture = json.loads((ROOT / "tests/quality/semantic.json").read_text())
    entries, queries = fixture["entries"], fixture["queries"]
    start = time.perf_counter()
    model = StaticModel.from_pretrained(args.model)
    load_ms = (time.perf_counter() - start) * 1000
    documents = model.encode([prepare(entry) for entry in entries])
    start = time.perf_counter()
    encoded = model.encode([query["text"] for query in queries])
    batch_ms = (time.perf_counter() - start) * 1000
    keys = [entry["key"] for entry in entries]
    lexical = quality.Engine(args.library, entries)
    try:
        lists = [lexical.query(query["text"]) for query in queries]
    finally:
        lexical.close()
    report = {"model": "minishlab/potion-retrieval-32M",
              "revision": "6fc8051fab2a1e0ee76689cf08c853792ac285e7",
              "weights_sha256": hashlib.sha256((pathlib.Path(args.model) / "model.safetensors").read_bytes()).hexdigest(),
              "entries": len(entries), "queries": len(queries), "load_ms": load_ms,
              "query_batch_ms": batch_ms, "peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
              "preprocessing": "launcher-text-1", "variants": {}}
    tuning = [i for i, query in enumerate(queries) if query["split"] == "tuning"]

    def summarize(results):
        buckets, rows = {}, []
        for query, found in zip(queries, results):
            values = quality.metrics(found, query["labels"])
            rows.append({**query, "top10": found[:10], "metrics": values})
            for group in ("all", query["class"]):
                buckets.setdefault(query["split"] + "/" + group, []).append(values)
        aggregate = {}
        for group, values in buckets.items():
            measures = set().union(*(value.keys() for value in values))
            aggregate[group] = {"queries": len(values), **{key: float(np.mean([v[key] for v in values if key in v])) for key in measures}}
        return {"aggregate": aggregate, "queries": rows}

    def objective(results):
        # Empty-query rejection is equally weighted with relevance, rather than
        # rewarding an unconditional nearest-neighbor result for every query.
        return np.mean([quality.metrics(results[i], queries[i]["labels"]).get("ndcg10", float(not results[i])) for i in tuning])

    report["variants"]["lexical"] = summarize(lists)
    for dimensions in (512, 256, 128):
        docs = normalized(documents[:, :dimensions])
        query_vectors = normalized(encoded[:, :dimensions])
        similarity = query_vectors @ docs.T
        quantized = np.round(docs * 127).astype(np.int8)
        quantized = normalized(quantized.astype(np.float32))
        quant_similarity = query_vectors @ quantized.T
        recall = []
        for a, b in zip(similarity, quant_similarity):
            recall.append(len(set(np.argsort(-a, kind="stable")[:10]) & set(np.argsort(-b, kind="stable")[:10])) / 10)
        candidates = []
        for threshold in (0.0, .15, .2, .25, .3, .35, .4, .45, .5, .55, .6):
            semantic = [[keys[j] for j in np.argsort(-row, kind="stable")[:10] if row[j] >= threshold] for row in similarity]
            for method in ("rrf", "weighted"):
                for weight in ((1.0,) if method == "rrf" else (.25, .5, .75, 1.0)):
                    results = []
                    for i, sem in enumerate(semantic):
                        lex = lists[i]
                        scores = {}
                        for ranking, multiplier in ((lex, 1.0), (sem, weight)):
                            for rank, key in enumerate(ranking, 1):
                                if method == "rrf":
                                    score = 1 / (60 + rank)
                                elif ranking is lex:
                                    score = 1 / rank
                                else:
                                    score = float(similarity[i, keys.index(key)])
                                scores[key] = scores.get(key, 0) + multiplier * score
                        # Exact full name/path wins independently of fusion.
                        text = queries[i]["text"].casefold()
                        exact = {e["key"] for e in entries if text in (e["path"].casefold(), e["path"].rsplit("/", 1)[1].casefold())}
                        found = sorted(scores, key=lambda key: (key not in exact, -scores[key], keys.index(key))) if sem else lex
                        results.append(found)
                    candidates.append((objective(results), threshold, method, weight, results))
        best = max(candidates, key=lambda row: (row[0], row[1], row[2] == "rrf"))
        selected = {"threshold": best[1], "method": best[2], "semantic_weight": best[3], "tuning_objective": float(best[0])}
        report["variants"][f"hybrid_{dimensions}"] = {"selection": selected, "int8_recall10": float(np.mean(recall)), **summarize(best[4])}
        raw = [[keys[j] for j in np.argsort(-row, kind="stable")[:10]] for row in similarity]
        report["variants"][f"semantic_{dimensions}"] = summarize(raw)
    pathlib.Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    for name, variant in report["variants"].items():
        print(name, json.dumps(variant["aggregate"]["heldout/all"], sort_keys=True))


if __name__ == "__main__":
    main()
