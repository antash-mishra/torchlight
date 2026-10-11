# mounts

> **Status:** Implemented (M7): mount-table snapshots and root-relevance comparison
> **Source:** `src/fs/mounts.c` · **Header:** `include/torchlight/mounts.h`
> **Tests:** `tests/unit/test_mounts.c`, `tests/unit/test_writer_repair.c`

## Purpose
The writer polls `/proc/self/mountinfo` because a mount can hide, reveal or
replace a whole tree without a single inotify event. Most mount-table changes
have nothing to do with the indexed roots, though: Timeshift's hourly check on
a stock Linux Mint desktop mounts a backup disk under `/run/timeshift/` and
unmounts it 17 s later, snaps mount squashfs images, containers come and go.
This module reads the table into a snapshot and answers one question: did a
mount appear or go at, below or above one of the roots?

## Responsibilities
- Read a mount table (bounded) and parse it into an owned, sorted snapshot.
- Compare two snapshots against a list of roots.
- It does **not** poll, schedule scans or know about the catalog; the
  [writer](../writer/README.md) does.

## Public API
| Function | Description |
|---|---|
| `mounts_parse(text, length, &out)` | Parse mountinfo text into an owned snapshot. |
| `mounts_read(path, &out)` | Read a table (at most `MOUNTS_MAX_BYTES`, 16 MiB) and parse it. |
| `mounts_destroy(mounts)` | Free a snapshot; NULL allowed. |
| `mounts_count(mounts)` | Number of mounts. |
| `mounts_affect(before, after, roots, count)` | Whether a mount in only one snapshot is at, below or above a root. |

Snapshots are owned by the caller; roots are borrowed for the call.

## Design
A mountinfo line reads `36 35 98:0 /mnt1 /mnt/parent rw,noatime master:1 -
ext3 /dev/root rw`. Its first five fields (mount id, parent id, device, root
within the filesystem, mount point) identify a mount: a remount that only
changes options keeps them, so it is no change, while a mount added or
removed, even one reusing a freed mount id or the same mount point with
another device, changes them. The snapshot stores those five fields verbatim
plus the decoded mount point, sorted by the verbatim fields, so comparison is
a merge of two sorted lists.

The kernel escapes a space, tab, newline or backslash in a mount point as a
backslash and three octal digits (`\040`); decoding keeps any other backslash
literal, so a path containing `\9` or `\000` survives. Relevance is
component-aware (`path_within`): a mount at `/home/mean` does not concern a
root at `/home/me`, while one at `/`, `/home`, `/home/me` or `/home/me/usb`
does.

`/proc` tables have no size until a read returns zero, so the reader grows a
buffer from 64 KiB to one byte past the 16 MiB limit, which tells a table at
the limit from a larger one (`TL_LIMIT`). A NUL byte, an empty field, a
non-numeric mount id, fewer than five fields or a relative mount point is
`TL_INVALID`.

## Data flow
The writer opens the table at start, reads the first snapshot, and after
each `POLLPRI` reads the next one and calls `mounts_affect` with its current
roots. Only `true` schedules a full scan (`last_full_reason: mount`); every
signal increments `mount_changes` in status. A read failure is treated as
relevant and leaves the stored snapshot NULL (unknown), so the next signal is
relevant too.

## Invariants
- Snapshot entries are sorted by their identity fields.
- `mounts_affect` with a NULL snapshot (unknown) returns true.

## Performance
A desktop's table is 25 to a few hundred lines: one read and a sort per
mount-table change, a few microseconds. Container hosts can reach thousands of
lines, still far below the 16 MiB bound. Nothing runs between changes.

## Testing
`test_mounts.c`: backup and snap mounts away from the root are ignored in both
directions; mounts below, at and above the root and a stacked mount over `/`
are relevant; a sibling sharing the root's prefix is not; a remount is no
change; a reused mount id elsewhere is a new mount; NULL snapshots and roots;
octal escapes (and escapes the kernel never writes); malformed lines; reading
the real `/proc/self/mountinfo`, a temporary file, a missing path and a table
past the limit. `test_writer_repair.c` drives the writer with a fake table.
`bench_scenarios.py`'s `mounts` phase mounts tmpfs in a private user and
mount namespace, outside and then inside the root.

## Gotchas
- A table read while mounts change can be inconsistent (the kernel builds it
  across read calls). Another signal follows the change, and the next read
  compares again; a spurious difference near a root only costs a full scan.
- Roots are compared as configured canonical paths. A mount reached through
  a symlinked root path is still found: the writer canonicalizes roots.

## Related
- Modules: [writer](../writer/README.md), [watch](../watch/README.md), [crawl](../crawl/README.md)
- ADRs: [0038](../../adr/0038-repair-scans-and-metadata-events.md), [0043](../../adr/0043-mount-changes-near-roots-and-watch-slot-reuse.md)
