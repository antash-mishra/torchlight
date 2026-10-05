# 0025. M4 cache staging retries

- **Status:** Accepted
- **Date:** 2026-10-06

## Context

Enabling Potion on the live catalog exposed repeated partial embedding builds.
Reconciliation held SQLite's writer transaction for longer than the cache's
three-second busy timeout. A failed cache BEGIN discarded the entire stage,
although no row in that batch had been processed. Periodic reconciliation could
therefore prevent a large first snapshot from becoming available.

## Decision

Preserve the stage and its position when a cache transaction cannot begin.
Return the failure to the worker, report it in semantic status and use the
existing timed condition wait before retrying. Interactive jobs can wake the
wait; repeated immediate errors cannot spin the worker at full CPU.

Continue checking catalog_gen and desktop_gen before retrying. Discard obsolete
stages normally. Keep existing cleanup after row/commit failures because row
text ownership and resident vectors can already have changed in those cases.

## Alternatives considered

Increasing busy timeout or reconciliation interval only moves the failure
threshold. Rebuilding an unchanged stage repeats work and can prevent first
publication indefinitely. Retrying a failed batch after processing rows requires
additional rollback handling and is outside this change.

## Consequences

An unchanged partial build resumes after cache writer contention clears while
lexical queries remain available. Snapshot memory stays within the existing
stage/active/retired bounds. A test shim injects repeated SQLITE_BUSY responses
only for the semantic worker's cache BEGIN; the regression checks retained
progress, lexical responses and eventual hybrid publication after release.
The same regression fails against the previous daemon.

Validation passed `make all`, `make test` with ASan/UBSan, `make lint` and
`make bench`. All 108 trained encoding parity cases and 52 quantized reference
order comparisons passed. Native held-out nDCG@10 remained 0.969 and top-ten
success remained 100% on the small labeled fixture.
