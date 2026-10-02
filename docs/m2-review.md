# M2 implementation review and fixes

Reviewed `0c97097` (resident snapshots) and `dc5fd51` (daemon, writer and live
updates) on 2026-10-03 against `PLAN.md` and ADRs 0010/0011.

Both reproduced failures below are now fixed, with sanitizer regression coverage
and successful reruns of the original reproductions. The separately documented
500k / 5 ms latency gate remains open. The design, schema migration and fallback
limits are recorded in [ADR 0012](adr/0012-filesystem-incarnations-and-watch-fallback.md).

## P1 (fixed): rapid replacement reused a stale file ID

**Source:** [`writer.c`](../src/service/writer.c), `changed`;
[`store.c`](../src/storage/store.c), `PUT_SQL`.

The reviewed writer retained paired moves, but ordinary delete/create
notifications only set a reconciliation flag. The scan upserted by path and
preserved the row's ID. A deletion followed by recreation before that scan kept
the deleted file's ID, even after publication. Resolve accepted the stale
selection and open history could attach to the replacement.

Reproduced with the sanitized daemon and the existing `Service` integration
harness in an isolated temporary root:

1. Index `reused.txt`, save its ID, and wait for initial reconciliation.
2. Hold an external `BEGIN IMMEDIATE` lock to prevent a scan transaction between
   the two filesystem operations. Delete the file and recreate it with different
   contents/size, then release the lock.
3. Wait for `catalog_gen` to advance, query the replacement, and resolve the old ID.

Observed: old ID `"2"`, replacement ID `"2"`, old-ID resolve `status: "ok"`,
`reason: "accepted"`. Expected: a fresh replacement ID and `stale_result` for
the deleted ID after reconciliation.

The fix records device/inode and birth time (ctime fallback) during crawling and
persists them in schema v2. Changed incarnations retire old ids and descendants
inside the scan transaction. History cascades belong to the retired object;
rollback restores them with the old ids. This also detects replacements during
downtime or without watch notifications once identities have been recorded.

The original reproduction now gives old ID `"2"`, replacement ID `"4"`, and
old-ID resolve `reason: "stale_result"`. New tests cover same-inode/different-birth
replacement, byte paths, directory descendants, SQL failure/retry, history,
restart repair and v1 migration. Legacy NULL identities are adopted on the first
successful scan. Without birth time, metadata changes can conservatively retire
ids/history too; paired renames retain device/inode while adopting a new ctime.

## P2 (fixed): inotify instance exhaustion prevented reconciliation

**Source:** [`writer.c`](../src/service/writer.c), `reconcile`;
[`watch.c`](../src/fs/watch.c), `watch_create`.

Every reviewed reconciliation required a fresh watcher before starting its scan
transaction. If `inotify_init1` failed, the writer returned early and retried the
same prerequisite, preventing periodic and explicit repair.

Reproduced by preloading a small test shim that makes `inotify_init1` return
`-1` with `errno = EMFILE`, simulating exhausted per-user inotify instances:

1. Create a saved catalog offline, then start the isolated daemon with the shim.
2. Create `unwatched.txt`; the harness configures a 500 ms rescan interval.
3. Wait three seconds and inspect query results and status.

Observed: the new file is absent, `reconciliations: 0`, `degraded: true`,
`watch_degraded: false`, and `watch_unavailable: 0`. Saved catalog queries work.
The failure occurs before each scan, so further retries remain blocked.

The fix treats TL_IO from watcher creation as reduced coverage, retains any old
watcher and continues the scan. Subsequent scans retry setup; successful setup
restores coverage. Memory and invalid-input errors still fail the batch.

The same preload reproduction now indexes the new file, with seven successful
reconciliations in the observation window, `watch_degraded: true` and
`watch_unavailable: 7`. A deterministic factory-failure test additionally checks
periodic publication, replacement detection without events, watcher recovery,
and preservation of the previous watch set on later creation failures.

## Verification and limits

- `make test`: passed module, CLI and daemon tests under ASan/UBSan. Unix socket
  tests required execution outside the filesystem/network sandbox.
- `make lint`: passed clang-tidy and cppcheck.
- Both focused reproductions completed without sanitizer diagnostics.
- `AGENTS.md` and `CLAUDE.md` are identical.
- The updated overview passed local-link/HTML checks and browser checks for
  keyboard tab switching and page overflow at 1280, 390 and 360 px widths during
  the initial review. Its fixed-status update also passed local-link/HTML checks.
- Performance figures in the overview come from the existing recorded release
  benchmarks in `docs/evaluation.md`; this documentation review did not rerun them
  or change the query path.

Snapshot pinning/reclamation, prepare/commit/publish ordering, rollback and
post-commit recovery, bounded clients, byte-safe transport, asynchronous history
and normal create/delete/rename flows have implementation and passing coverage.
The current suite includes regression coverage for both failures above.
