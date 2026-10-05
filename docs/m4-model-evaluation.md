# M4 model selection and evaluation

The provisional English-first backend is **minishlab/potion-retrieval-32M**, with
256-dimensional normalized output and exhaustive int8 cosine retrieval. It is
implemented in C and tested through the resident daemon. M4's performance and
broader model-comparison acceptance remains open; semantics is opt-in.

## Research

| Model | Architecture / output | License | Assessment |
|---|---|---|---|
| [Potion retrieval 32M](https://huggingface.co/minishlab/potion-retrieval-32M) | Static WordPiece table and mean pooling; 512 original dimensions | MIT | Tested here; native implementation uses existing dependencies. Short name/metadata inference is inexpensive, with limited word-order context. |
| [Granite small English R2](https://huggingface.co/ibm-granite/granite-embedding-small-english-r2) | ModernBERT contextual encoder; 384 dimensions, 47M parameters | Apache-2.0 | Researched contextual alternative. Would require a transformer runtime and separate C tokenizer/inference parity; no local quality comparison has been claimed. |
| [BGE small English v1.5](https://huggingface.co/BAAI/bge-small-en-v1.5) | BERT contextual encoder; 384 dimensions, approximately 33M parameters | MIT | Researched established baseline; likewise requires a transformer backend. Not locally benchmarked in this change. |

Publisher retrieval benchmarks are not launcher relevance measurements. Potion
was selected for the requested first small experiment, not declared the universal
winner. [Model2Vec inference](https://github.com/MinishLab/model2vec) provides the
reference. An autoregressive generator is unnecessary for returning ranked file
and application ids. Contextual embeddings can be compared later through the
existing adapter, with any new runtime discussed before addition.

Pinned revision: `6fc8051fab2a1e0ee76689cf08c853792ac285e7`.
Source safetensors SHA256:
`07609e5bd33aad37900b3fd62f4ec96f6daec88ca4d46b9d8b928bfababf6ea0`.
The exporter rejects other weights and unsupported tokenizer recipes. The 256d
native package is 65,349,928 bytes. Header hashes record source/tokenizer identity;
payload SHA256 detects corruption. The model's Matryoshka training permits an
explicit truncation experiment; no new PCA was fitted on evaluation data.

## Small labeled experiment

[semantic.json](../tests/quality/semantic.json) has 36 file/application entries
and 52 queries: 24 semantic paraphrases, 24 names and four no-match controls.
Target families are assigned to tuning/held-out splits before evaluation. Inputs
are names, extensions, nearby parent folders and explicit app metadata; contents
are not read. Labels are manually constructed toy examples, not a representative
home directory. The fixture deliberately tests concepts lexical search cannot
match; therefore the measured gain should not be generalized to all typing.

The offline comparison tests 512/256/128 dimensions, semantic-only, lexical,
RRF and a weighted reciprocal lexical-rank/cosine combination. Threshold and
weight selection uses tuning rows only. RRF k=60/cosine 0.2 won at 256d. The
256d result matched 512d held-out relevance; 128d was weaker.

| Held-out labeled queries | Lexical | Offline float hybrid | Native daemon/int8 hybrid |
|---|---:|---:|---:|
| nDCG@10, 24 relevant queries | 0.500 | 0.985 | 0.969 |
| MRR | 0.500 | 0.979 | 0.958 |
| Top-ten success | 50% | 100% | 100% |
| No-match correctness, two queries | 100% | 100% | 100% |

The native experiment includes real directory entries, unlike the original
36-item offline catalog. A temporary parent outside indexing scope initially
introduced a false positive. Scoped preprocessing now starts at the indexed
root's basename; the fix has a regression test. Thresholds were not retuned on
held-out rows. Some tuning intents remain weak (team discussion, music-player
selection), and graded ordering differs with directory competitors. Candidate
recall differs in meaning: the offline lexical list returns up to 1000 entries;
the native end-to-end report labels only the displayed ten.

Native small-catalog publication took about 583 ms. On the shared reference host,
lexical round-trip p95 was 0.114 ms and final p95 0.142 ms, with about 74,844 KiB
resident/peak process RSS. These are small-fixture timings, not 500k results.
Artifacts: [offline](../tests/quality/results/2026-10-05-potion.json),
[native daemon](../tests/quality/results/2026-10-05-potion-daemon.json).

## Parity and representation

[Native parity](../tests/quality/results/2026-10-05-potion-parity.json) covers 108
fixture/Unicode/control/subword/special-token cases. Normalized native components
match float32-rounded Model2Vec reference output within 2e-6 (this run was exact
after float32 rounding). This is finite test coverage, not every Unicode sequence.
Measured encode timings include ctypes overhead and host contention; actual values
are recorded in the artifact. C encoding allocates no memory or performs I/O.

The native int8 top ten matches an independent NumPy quantized reference on all
52 fixture queries. Mean Recall@10 against float is 0.97885; maximum cosine error
against the quantized oracle is 1.36e-7. Quantization can change neighbors and
requires broader evaluation. The float reference remains available separately.

The [exhaustive int8 scan benchmark](../tests/bench/results/2026-10-05-m4-int8.txt)
uses synthetic 256d vectors, 64 warm searches, top ten, no inference or IPC.
A [final-format rerun](../tests/bench/results/2026-10-05-m4-native-int8.txt)
records 13,400,104 / 134,000,104 bytes and p95 12.832 / 99.770 ms; shared-host
variation matters. Initial measurements below are retained for comparison:

| Rows | Index bytes | Scan p50 / p95 / p99 ms |
|---:|---:|---:|
| 50k | 13,400,080 | 5.887 / 11.495 / 14.501 |
| 500k | 134,000,080 | 69.717 / 84.138 / 101.400 |

A bounded experimental sign-bit Hamming shortlist and int8 rescoring is also
implemented, with portable bit counts and runtime-checked POPCNT acceleration.
Its stored format fits 150 MiB at 500k (150,000,104 bytes). Two-pass histogram
selection avoids candidate heap allocations and handles Hamming ties by id.
It is **not enabled in the service**: actual Potion embeddings of 500k synthetic
paths, 128 deterministically sampled queries, gave only 0.9445 mean Recall@10
with a 10k shortlist (p95 15.0 ms), and 0.9648 with a 20k shortlist (p95 18.9 ms),
against exhaustive int8. These miss the experiment's 0.98 mean recall gate and
10 ms latency target. Recall here compares neighbors, not human relevance.
Artifacts: [10k shortlist](../tests/quality/results/2026-10-05-potion-binary-500k.json),
[20k shortlist](../tests/quality/results/2026-10-05-potion-binary-20k-500k.json).

## Resident scale and updates

[Daemon benchmark](../tests/bench/results/2026-10-05-m4-hybrid-daemon.jsonl):
1200 sequential synthetic launcher queries per size, pinned Potion/native CPU,
exhaustive int8, fixed 1000-entry lexical pool, actual local IPC and background
cache. Original M1 corpus seed 42/query seed 2; duplicates are removed. The
benchmark uses a 1000 ms final deadline to measure full scans rather than censor
slow searches at the default 200 ms. Shared/unpinned i7-8700K host, Linux/GCC13,
C17 `-O3 -DNDEBUG`. The large run preceded the scoped-context correction; it
measures implementation costs, not corrected semantic relevance.

| Requested / catalog entries | Lexical p95 ms | Terminal p95 ms | First semantic publication ms | Steady / peak-after-update KiB |
|---|---:|---:|---:|---:|
| 50k / 49,569 | 2.402 | 12.333 | 17,651 | 128,472 / 169,064 |
| 500k / 494,362 | 12.723 | 100.442 | 464,693 | 589,836 / 972,436 |

File publication after 101 real additions was approximately 433 / 4373 ms;
those numbers measure lexical catalog update lag, not semantic vector refresh
completion. During changes the old semantic view is unavailable for a newer
catalog and queries fall back to lexical until staging completes. Model/cache
validation, metadata/vector staging and retained snapshots explain the additional
RSS. The fixed lexical pool also costs more than displaying only ten results.
Neither the original lexical 5 ms nor final 10 ms target is established at 500k.
Initial embedding/cache throughput and complete semantic-update lag remain
optimization/measurement work. Approximate retrieval was evaluated and rejected
rather than accepted on speed alone.

## User Documents check

The requested Documents folder contains 42 files. Five are under hidden
directories excluded by the crawler's existing default policy. All **37/37**
eligible exact-name searches kept the correct first result and coherent two
phases. This is an automatically generated name-preservation smoke check, not a
human-labeled semantic evaluation. Names and individual results are not stored
in git; contents were not read. [Aggregate artifact](../tests/quality/results/2026-10-05-documents-summary.json).
A private neighbor check on all 42 names also matched exhaustive retrieval;
with fewer items than the shortlist, it does not test approximate pruning.

## Reproduce

Create an isolated Python environment and install `model2vec==0.9.0`. Download
the pinned model's config, tokenizer and safetensors into a local directory; keep
weights out of git. No runtime download occurs in Torchlight.

```sh
make build/quality_engine.so build/torchlightd-release
python scripts/export_potion.py --model /path/to/pinned-model --output build/models/potion-256.tlm
python scripts/evaluate_semantic.py --model /path/to/pinned-model --output /tmp/offline.json
python scripts/check_potion_parity.py --model /path/to/pinned-model --native build/models/potion-256.tlm --output /tmp/parity.json
python scripts/evaluate_semantic_daemon.py --model build/models/potion-256.tlm --output /tmp/daemon.json
python scripts/check_local_search.py --root "$HOME/Documents" --model build/models/potion-256.tlm --output /tmp/documents-summary.json
```

The optional tools require Python packages only in that evaluation environment.
Normal `make test` uses analytic models, sanitizer fault injection and an actual
terminal CLI exchange; it needs no weights or Python ML package.
The current change also passed `make all`, `make lint`, `make bench` and
`make bench-vector`; [lexical artifact](../tests/bench/results/2026-10-05-m4-native-lexical.txt)
and [float reference artifact](../tests/bench/results/2026-10-05-m4-native-float.txt)
retain the engine regression/performance checks.
