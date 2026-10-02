# 0012. Persist filesystem incarnations and reconcile without inotify

- **Status:** Accepted
- **Date:** 2026-10-03
- **Refines:** 0009 and 0011

## Context

The M2 review found two failures. Coalescing delete/create notifications into a
scan preserved the deleted object's path-based id. Requiring a new inotify
instance before every scan prevented all repair during instance exhaustion.
Replacement detection also needs to work after missed events or a restart.

## Decision

The crawler reports a filesystem incarnation: device, inode and a nanosecond
birth timestamp from `statx`, without following symlinks. Metadata and identity
come from the same fresh observation. Unsupported birth time uses ctime instead;
unsupported statx uses the fts stat's device/inode/ctime. Other stat failures
mark the scope unreadable and retain saved entries.

Schema v2 adds a nullable, fixed-width BLOB `files.identity` through an atomic
migration. The 29-byte encoding is independent of ABI and host endianness:

| Bytes | Value |
|---|---|
| 0 | Kind: 0 = ctime, 1 = birth time, 2 = paired rename awaiting a fallback stamp |
| 1–8 | Device, unsigned 64-bit little-endian |
| 9–16 | Inode, unsigned 64-bit little-endian |
| 17–24 | Timestamp seconds, signed 64-bit encoded little-endian |
| 25–28 | Nanoseconds, unsigned 32-bit little-endian |

A prepared per-path lookup compares identities before upserting. A changed
incarnation deletes the old row and descendants, clears their temporary
seen/kept membership, and allocates fresh AUTOINCREMENT ids. Cascading open
history belongs to the retired object. All this is inside the existing scan
transaction: failed preparation/SQL rolls back ids, identity and history; readers
keep the previous published view until the replacement commits and publishes.
Unchanged birth identities preserve ids across metadata updates.

Paired moves preserve ids and the object key. Birth timestamps stay unchanged;
ctime fallback stamps are reset to kind 2, retaining device/inode for validation
when the following scan adopts the new stamp. A different device/inode still
retires the moved row. Unpaired moves remain delete/add reconciliation.

Legacy v1 rows migrate with NULL identity and keep their ids/history. Their first
successful stat adopts an identity without retiring the row. This bootstrap
cannot detect a replacement that happened before an identity was recorded.

An inotify factory failure returning TL_IO records watch degradation and an
unavailable attempt, then continues scanning. Keep the old watcher when present;
replace it only after successfully building a new one. Periodic and explicit
scans retry watcher setup while remaining able to commit/publish catalog changes.
Memory/invalid-input errors still fail the batch. A factory adapter permits
deterministic instance-failure and recovery tests without production test modes.

## Alternatives considered

- Retain only delete notifications: cannot repair identity after overflow,
  unavailable watches, or downtime, and requires ordered replay with renames.
- Device/inode alone: an inode can be reused after deletion.
- Ctime everywhere: regular writes and chmod would unnecessarily retire ids on
  filesystems that provide stable birth timestamps.
- Require inotify for every scan: makes the fallback depend on the failed resource.

## Consequences

Both reproduced failures have sanitizer regressions, including directory
replacement, rollback/history restoration, restart repair, migration, periodic
scan-only operation, watcher recovery and retention of an existing watcher.
Queries still use resident snapshots without SQL, filesystem reads or allocation.
No dependency is added. Crawling adds a statx call and a prepared identity lookup
per indexed path; full engine rebuilding and the 500k latency gate remain open.

Without birth time, ctime is conservative: ordinary metadata changes can retire
ids and their open history; directory changes can also retire descendant ids.
Paired renames retain object keys, but unavailable watches cannot prove rename
pairing. Filesystem timestamps and inode identity are observations at scan time;
the catalog is eventually consistent, not an atomic guard around an external
launch. Unreadable/offline scopes continue retaining the last saved view.
