#!/usr/bin/env python3
"""Measure sign shortlist recall against exhaustive int8 on trained corpus vectors.

Input NUL-separated paths and JSON-line launcher queries from bench_fixture.
This is a reference-neighbor comparison, not human relevance labels.
"""
import argparse
import ctypes as C
import json
from pathlib import Path
import random
import time
import numpy as np
from model2vec import StaticModel
from evaluate_semantic import normalized, prepare


class Hit(C.Structure):
    _fields_ = [('id', C.c_uint64), ('cosine', C.c_double)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', required=True)
    parser.add_argument('--paths', required=True)
    parser.add_argument('--queries', required=True)
    parser.add_argument('--library', default='build/quality_engine.so')
    parser.add_argument('--shortlist', type=int, default=10000)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    paths = [x.decode('utf-8', 'replace') for x in Path(args.paths).read_bytes().split(b'\0') if x]
    queries = [json.loads(x)['query'] for x in Path(args.queries).read_text().splitlines()]
    queries = random.Random(42).sample(queries, min(128, len(queries)))
    model = StaticModel.from_pretrained(args.model)
    values = np.empty((len(paths), 256), dtype=np.float32)
    start = time.perf_counter()
    for begin in range(0, len(paths), 1024):
        texts = [prepare(dict(path=p, kind='file')) for p in paths[begin:begin + 1024]]
        values[begin:begin + len(texts)] = normalized(model.encode(texts)[:, :256])
    encode_seconds = time.perf_counter() - start
    vectors = normalized(model.encode(queries)[:, :256]).astype(np.float32)
    library = C.CDLL(str(Path(args.library).resolve()))
    signatures = {
        'vector_create_int8': ([C.c_uint64, C.c_size_t, C.c_size_t, C.c_size_t, C.POINTER(C.c_void_p)], C.c_int),
        'vector_create_binary_int8': ([C.c_uint64, C.c_size_t, C.c_size_t, C.c_size_t, C.c_size_t, C.POINTER(C.c_void_p)], C.c_int),
        'vector_add': ([C.c_void_p, C.c_uint64, C.c_uint64, C.POINTER(C.c_float), C.c_size_t], C.c_int),
        'vector_finish': ([C.c_void_p], C.c_int),
        'vector_workspace_create': ([C.c_void_p, C.POINTER(C.c_void_p)], C.c_int),
        'vector_query': ([C.c_void_p, C.c_void_p, C.c_uint64, C.POINTER(C.c_float), C.c_size_t, C.POINTER(Hit), C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
        'vector_destroy': ([C.c_void_p], None),
        'vector_workspace_destroy': ([C.c_void_p], None),
        'vector_bytes': ([C.c_void_p], C.c_size_t),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(library, name)
        function.argtypes, function.restype = arguments, result
    reference, approximate = C.c_void_p(), C.c_void_p()
    workspaces = [C.c_void_p(), C.c_void_p()]
    assert library.vector_create_int8(1, 256, len(paths), 150 * 1024 * 1024, C.byref(reference)) == 0
    assert library.vector_create_binary_int8(1, 256, len(paths), args.shortlist, 150 * 1024 * 1024, C.byref(approximate)) == 0
    try:
        for number, vector in enumerate(values, 1):
            if np.linalg.norm(vector) == 0:
                continue
            pointer = vector.ctypes.data_as(C.POINTER(C.c_float))
            for index in (reference, approximate):
                assert library.vector_add(index, number, 1, pointer, 256) == 0
        for index, workspace in zip((reference, approximate), workspaces):
            assert library.vector_finish(index) == 0
            assert library.vector_workspace_create(index, C.byref(workspace)) == 0
        rows, reference_ms, approximate_ms = [], [], []
        for text, vector in zip(queries, vectors):
            if np.linalg.norm(vector) == 0:
                continue
            rankings = []
            for index, workspace, samples in zip((reference, approximate), workspaces, (reference_ms, approximate_ms)):
                hits, count = (Hit * 10)(), C.c_size_t()
                start = time.perf_counter()
                assert library.vector_query(index, workspace, 1, vector.ctypes.data_as(C.POINTER(C.c_float)), 256, hits, 10, C.byref(count)) == 0
                samples.append((time.perf_counter() - start) * 1000)
                rankings.append([hit.id for hit in hits[:count.value]])
            rows.append(dict(query=text, recall10=len(set(rankings[0]) & set(rankings[1])) / 10,
                             top1_agreement=rankings[0][:1] == rankings[1][:1]))
        report = dict(rows=rows, paths=len(paths), dimensions=256, shortlist=args.shortlist,
                      encode_seconds=encode_seconds, mean_recall10=float(np.mean([r['recall10'] for r in rows])),
                      top1_agreement=float(np.mean([r['top1_agreement'] for r in rows])),
                      reference_ms={str(p): float(np.percentile(reference_ms, p)) for p in (50, 95, 99)},
                      approximate_ms={str(p): float(np.percentile(approximate_ms, p)) for p in (50, 95, 99)},
                      approximate_bytes=library.vector_bytes(approximate),
                      limits='Trained embeddings of synthetic paths; no human labels, no IPC/inference in search timing.')
        Path(args.output).write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: v for k, v in report.items() if k != 'rows'}))
    finally:
        for workspace in workspaces:
            library.vector_workspace_destroy(workspace)
        for index in (reference, approximate):
            library.vector_destroy(index)


if __name__ == '__main__':
    main()
