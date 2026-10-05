#!/usr/bin/env python3
"""Compare native C encoding with the pinned Model2Vec implementation.

Run after export_potion.py and make build/quality_engine.so. Includes the
complete labeled fixture plus normalization/subword/special-token edge cases.
No network; large trained weights remain outside version control.
"""
import argparse
import ctypes as C
import json
import pathlib
import time

import numpy as np
from model2vec import StaticModel
from evaluate_semantic import ROOT, normalized, prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--native", required=True)
    parser.add_argument("--library", default="build/quality_engine.so")
    parser.add_argument("--dimensions", type=int, default=256)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    fixture = json.loads((ROOT / "tests/quality/semantic.json").read_text())
    texts = [prepare(entry) for entry in fixture["entries"]]
    texts += [query["text"] for query in fixture["queries"]]
    texts += ["Café CAFÉ cafe\u0301", "naïve résumé Ångström", "Straße STRASSE İ I ı",
              "hello\tworld\nhello\rworld", "abc\x01def abc\u200ddef",
              "中文 日本語 한국어", "a+b=c /tmp/a_b.txt", "😀 hello 🐈",
              "hello[CLS]world[SEP]", "[MASK] [PAD] [UNK]", "[cls] unknown",
              "coöperation re-enter can't", "a" * 100, "a" * 101,
              "the the the the", "alpha\u00a0beta", "hello\u2028world",
              "ﬃ ﬁ Æther", "ΩΣς", "𠀀 english"]
    reference = StaticModel.from_pretrained(args.model)
    expected = normalized(reference.encode(texts)[:, :args.dimensions]).astype(np.float32)
    library = C.CDLL(str(pathlib.Path(args.library).resolve()))
    library.potion_load.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(C.c_void_p)]
    library.potion_load.restype = C.c_int
    library.embedder_encode.argtypes = [C.c_void_p, C.c_int, C.c_char_p,
                                       C.POINTER(C.c_float), C.c_size_t]
    library.embedder_encode.restype = C.c_int
    library.embedder_destroy.argtypes = [C.c_void_p]
    model = C.c_void_p()
    start = time.perf_counter()
    status = library.potion_load(args.native.encode(), 256 * 1024 * 1024, C.byref(model))
    assert status == 0, status
    load_ms = (time.perf_counter() - start) * 1000
    errors, times = [], []
    try:
        for text, target in zip(texts, expected):
            output = np.zeros(args.dimensions, dtype=np.float32)
            start = time.perf_counter()
            status = library.embedder_encode(model, 0, text.encode(),
                                            output.ctypes.data_as(C.POINTER(C.c_float)), args.dimensions)
            times.append((time.perf_counter() - start) * 1e6)
            if np.linalg.norm(target) == 0:
                assert status == 5 and not output.any(), (text, status)
            else:
                assert status == 0, (text, status)
                error = float(np.max(np.abs(output - target)))
                errors.append(error)
                assert error < 2e-6, (text, error, reference.tokenize([text]))
    finally:
        library.embedder_destroy(model)
    report = {"cases": len(texts), "dimensions": args.dimensions,
              "maximum_absolute_error": max(errors), "tolerance": 2e-6,
              "native_load_ms": load_ms,
              "native_encode_us": {str(p): float(np.percentile(times, p)) for p in (50, 95, 99)},
              "includes_ctypes_overhead": True}
    class Hit(C.Structure):
        _fields_ = [("id", C.c_uint64), ("cosine", C.c_double)]
    signatures = {
        'vector_create_int8': ([C.c_uint64, C.c_size_t, C.c_size_t, C.c_size_t, C.POINTER(C.c_void_p)], C.c_int),
        'vector_add': ([C.c_void_p, C.c_uint64, C.c_uint64, C.POINTER(C.c_float), C.c_size_t], C.c_int),
        'vector_finish': ([C.c_void_p], C.c_int),
        'vector_workspace_create': ([C.c_void_p, C.POINTER(C.c_void_p)], C.c_int),
        'vector_query': ([C.c_void_p, C.c_void_p, C.c_uint64, C.POINTER(C.c_float), C.c_size_t, C.POINTER(Hit), C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
        'vector_destroy': ([C.c_void_p], None),
        'vector_workspace_destroy': ([C.c_void_p], None),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(library, name)
        function.argtypes, function.restype = arguments, result
    index, workspace = C.c_void_p(), C.c_void_p()
    entries = len(fixture['entries'])
    documents = expected[:entries]
    assert library.vector_create_int8(1, args.dimensions, entries, 150 * 1024 * 1024, C.byref(index)) == 0
    try:
        for number, values in enumerate(documents, 1):
            values = np.ascontiguousarray(values)
            status = library.vector_add(index, number, 1, values.ctypes.data_as(C.POINTER(C.c_float)), args.dimensions)
            assert status == 0, (number, status, values.dtype, np.linalg.norm(values))
        assert library.vector_finish(index) == 0
        assert library.vector_workspace_create(index, C.byref(workspace)) == 0
        quantized = np.sign(documents) * np.floor(np.abs(documents) * 127 + .5)
        quantized = normalized(quantized)
        recall, score_errors = [], []
        for values in expected[entries:entries + len(fixture['queries'])]:
            values = np.ascontiguousarray(values)
            hits, count = (Hit * 10)(), C.c_size_t()
            assert library.vector_query(index, workspace, 1, values.ctypes.data_as(C.POINTER(C.c_float)), args.dimensions, hits, 10, C.byref(count)) == 0
            scores = quantized @ values
            reference_order = np.argsort(-scores, kind='stable')[:10]
            actual_order = [hit.id - 1 for hit in hits[:count.value]]
            assert actual_order == list(reference_order), (actual_order, reference_order)
            score_errors += [abs(hit.cosine - float(scores[hit.id - 1])) for hit in hits[:count.value]]
            float_order = np.argsort(-(documents @ values), kind='stable')[:10]
            recall.append(len(set(actual_order) & set(float_order)) / 10)
        report['native_int8'] = {'reference_order_cases': len(recall), 'float_recall10': float(np.mean(recall)), 'maximum_cosine_error': max(score_errors)}
    finally:
        library.vector_workspace_destroy(workspace)
        library.vector_destroy(index)
    pathlib.Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
