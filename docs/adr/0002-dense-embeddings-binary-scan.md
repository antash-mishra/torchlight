# 0002. Static embeddings via ONNX + binary-quantized brute-force scan

- **Status:** Superseded by 0003
- **Date:** 2026-10-02

## Context
We want semantic matching (`tax receipts` → `ITR_2024_ack.pdf`) within a
~10ms query budget on ~500k paths, using ONNX Runtime from C.

## Decision
The initial proposal selected `minishlab/potion-retrieval-32M` with a binary
scan and int8 rescoring of 200 candidates. These choices are now experiments,
pending filename quality, C inference parity, recall, memory, and latency
measurements in M4. ADR 0003 replaces the unbenchmarked default.

## Alternatives considered
- **HNSW:** requires tuning and incremental maintenance. Compare if measured
  brute-force latency or recall requires ANN; scan timing is not yet established.
- **Transformer by default (bge-small, granite-30m):** better quality but much
  slower to index. Kept as an option.
- **sqlite-vec:** puts SQLite on the query path.

## Consequences
- Full-home indexing time and total query latency require measurements.
- General model benchmarks do not establish filename quality or prove that RRF
  and personalization compensate for differences between models.
- Memory must include int8 vectors, models, paths, postings, and generations.
