# Mixed launcher relevance evaluation

`mixed.json` contains realistic application/settings names, noisy competing
files, incomplete edits, abbreviations, duplicate names, folder/name queries,
extensions, normalized Unicode, malformed bytes and no-match queries. Grades
are 3 (preferred) and 1 (reasonable alternative). Target families and their
variants stay together in the explicit tuning/held-out split. The two alignment
families are controlled comparisons rather than real application names.

The native evaluator merges separate desktop and file engines using the daemon's
score-first behavior. It verifies fresh-workspace and top-k equivalence. The
fixture has fewer than 1000 entries, so returned membership is complete, making
candidate recall exact here. Large synthetic benchmark recall@1000 is a ranked
proxy, not complete pre-ranking recall. Missing useful results have first rank
1001 and MRR 0; no-match queries have a separate empty-result accuracy measure
and do not enter relevance averages.

BM25 uses exact case-folded NFC word tokens, separate fields, k1=1.2, b=0.75,
and weights name/generic/keyword/parent = 3/1.5/1/0.5. It deliberately has no
prefix, typo or fuzzy expansion. It is a comparison, not a new runtime dependency.

Build the native library with `make build/quality_engine.so`, then run:

```sh
python3 tests/quality/evaluate.py \
  --baseline /tmp/torchlight-m3p2-baseline.so \
  --current build/quality_engine.so \
  --output /tmp/search-quality.json
```

The recorded baseline is revision `7ace90f0a3d1474a0594b08ee8db9fde72e0f713`.
To reproduce it, export that revision into a temporary directory using
`git archive`, copy the current Makefile (which adds the shared-library target)
there, and run `make build/quality_engine.so` in that directory. Supply the same
machine.mk or dependency overrides as the current build when needed. Save that
library as the baseline path above; no tracked source needs modification.

The evaluator fails unless held-out nDCG and candidate recall improve. Keep the
baseline frozen; do not move query variants between splits after measuring them.
Use a larger independently labeled corpus before drawing broad quality claims.

## M4 English semantic fixture

`semantic.json` contains 36 items and 52 queries with target-family splits.
`scripts/evaluate_semantic.py` compares local pinned Potion dimensions/fusion;
`check_potion_parity.py` checks C/reference inference and int8 neighbors;
`evaluate_semantic_daemon.py` evaluates the actual two-phase resident path.
See [model evaluation](../../docs/m4-model-evaluation.md). Normal tests do not
require model weights. Private Documents names are not committed.
