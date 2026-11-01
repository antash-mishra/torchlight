# Torchlight footprint investigation and optimization plan

**Status:** Approved and implemented on 2026-11-01. See the
[implementation record](footprint-implementation.md) for changes, validation,
live cleanup and remaining targets. The baseline and proposed sequence below
are retained as the original investigation against commit `20f4c6c`; statements
about deployment below describe that earlier point in time.

The largest disk cost is an unused semantic-search cache, followed by its model
file. The executables also contain substantial unused code. Address those first.
For RAM, prioritize the popup's graphics resources and lifetime before changing
the daemon's search indexes. The current daemon already has low background CPU
use, so search speed and correctness should remain constraints throughout.

## Measured baseline

This is the existing Mint installation, with 37,974 indexed file/directory
entries, 6,816 watches and 3,528 available apps. Semantic search is disabled
(`emb_gen` is null). These figures describe accumulated data on this workstation,
not the space required by a fresh install. Shared system libraries and external
Flatpak/Software installations are excluded from the disk totals.

### Disk

| Component | MiB | Finding |
|---|---:|---|
| `embedding_cache` table | 491.91 | 109,415 historical vectors, unused by the current lexical-only daemon |
| Free pages inside `catalog.db` | 51.67 | Reusable by SQLite, still occupying disk |
| `files` table and path index | 11.27 | The actual file-search catalog |
| Other database tables/indexes | about 0.18 | History and metadata are small |
| **Entire database** | **555.03** | Includes all four rows above |
| Optional Potion model file | 62.32 | Present on disk, not loaded by this daemon |
| Database WAL | 3.94 | Normal write journal; size varies |
| Three installed executables | 32.98 | Includes an unstripped popup installed during the recent update |
| Fonts and desktop integration | about 0.4 | Low priority |
| **Total owned footprint** | **about 655** | Database, model, executables and integration |

The database measurements used read-only `dbstat` and aggregate counts, not a
copy of private filenames or history. `dbstat` excludes free-list pages, which
were counted separately. See [SQLite's dbstat documentation](https://www.sqlite.org/dbstat.html).

### Memory and CPU

| Process | RSS, MiB | PSS, MiB | Anonymous PSS, MiB |
|---|---:|---:|---:|
| Daemon | 64.2 | 58.2 | 56.5 |
| Popup | 160.7 | 77.7 | 36.2 |
| **Combined** | **224.9** | **135.9** | **92.7** |

RSS counts shared mappings in each process. PSS divides shared pages among
their users and is the more useful combined RAM budget here; both vary as
other applications start or stop. See the [Linux process-memory documentation](https://docs.kernel.org/filesystems/proc.html).

A separate per-mapping sample attributed about **99 MiB RSS / 43 MiB PSS** of
the popup to graphics-driver mappings, including NVIDIA, Mesa/LLVM and GL/EGL
libraries. This is mapped system RAM, not a measurement of GPU VRAM. The
popup's service reported roughly 1.16 GiB because its cgroup also contained
Spotify processes and charged file cache. That service total must not be
reported as Torchlight's own RAM.

Two passive CPU-counter samples 534.89 seconds apart showed 0.89 CPU-seconds
for the daemon and 0.01 for the popup: **0.166% and 0.002% of one core**.
Waited-for daemon children added 1.45 CPU-seconds, taking daemon plus those
children to about **0.44%**. This was normal desktop observation, not a
controlled idle benchmark; future measurements must include background helpers.

## Causes and experiments

### Semantic cache storage layout

[The schema](../src/storage/store.c) stores 1,024-byte float vectors plus their
keys in a `WITHOUT ROWID` table with 4 KiB pages. Its measured payload is only
113.14 MiB, while unused space within its allocated pages is 377.33 MiB.
This is distinct from the database's 51.67 MiB of entirely free pages.

SQLite stores `WITHOUT ROWID` rows in index B-trees; the row size crosses the
local-payload limit and spills into poorly occupied overflow pages. Its own
guidance favors smaller rows for this layout. See the
[SQLite file format](https://www.sqlite.org/fileformat2.html) and
[WITHOUT ROWID guidance](https://www.sqlite.org/withoutrowid.html).

A Boat experiment used 109,415 synthetic records, 1,024-byte payloads and
36-byte synthetic keys. No live database or private data left the workstation.

| Synthetic layout | Size after compaction, MiB |
|---|---:|
| Existing layout, 4 KiB pages, `WITHOUT ROWID` | 480.84 |
| Ordinary rowid table, same primary-key uniqueness, 4 KiB pages | 149.36 |
| `WITHOUT ROWID`, 8 KiB pages | 122.14 |

Compaction alone changed the original synthetic database from 488.47 to
480.84 MiB. It does not fix the overflow layout. The alternatives demonstrate
storage savings, not a completed migration or a measured lookup-speed win.

Cache pruning currently happens when an embedding generation is activated in
`store_embedding_activate`. Disabling semantic search avoids using the cache
but does not retire its stored vectors. Preserve the option to reuse them when
semantics is enabled again; do not silently purge every disabled user's cache.

### Executable packaging and linking

[The release recipes](../Makefile) compile the full source list into each
executable. The installed popup has about 6.52 MiB of Frizbee text symbols even
though it uses IPC for searches. Release binaries include debug information;
the normal [portable packaging step](../packaging/portable/build_tarball.sh)
strips it, but the recently installed popup bypassed that step.

| Executable | Installed, MiB | Strip only, MiB | Section-GC prototype, MiB |
|---|---:|---:|---:|
| CLI | 8.99 | 8.99 | 3.25 |
| Daemon | 8.99 | 8.99 | 3.40 |
| Popup | 15.00 | 9.13 | 0.53 |
| **Total** | **32.98** | **27.12** | **7.18** |

The prototype used `-ffunction-sections -fdata-sections` and linker
`--gc-sections`, then stripped the output. It retained the existing source,
portable static dependencies and cached Frizbee archive. CLI, search-contract
and native Web/software action checks passed against the prototype. Full release
validation and performance comparison remain implementation gates. The installed binaries
were not replaced. Stripping debug information primarily saves disk space;
it should not be counted as equivalent RSS savings.

### Popup and daemon residency

[Closing the popup](../ui/gtk/launcher.c) cancels active work and hides the
window, retaining the GTK application for fast reopening. Graphics resources
and loaded drivers are therefore a better first RAM target than the small
result list. A renderer or lifetime change needs an actual memory/latency
comparison, especially on NVIDIA; no RAM saving has yet been demonstrated.

The daemon's approximately 58 MiB PSS includes resident file/application indexes,
query workspaces, the available-app index, watcher state and persistence state.
These are candidates for allocation attribution, not measured per-module
totals. Previous M7 work already added heap release and incremental indexing;
do not repeat the removed deletion-compaction policy that increased update lag.

## Implementation sequence

| Order | Work | Expected benefit | Completion gate |
|---|---|---|---|
| 1 | Use stripped staged installs and enable section garbage collection in release builds | Executables near the measured 7.2 MiB, about 78% below this installed build | Packaged binaries under 8 MiB total; all supported launch/search paths pass |
| 2 | Add explicit maintenance for unused semantic cache and managed model files, with compaction | Largest saving for lexical-only users; about 544 MiB of cache/free pages plus the optional 62 MiB model here | Preserve file IDs, roots and history; recover safely from interruption; verify compacted size |
| 3 | Migrate retained semantic caches to a layout suited to vector payloads and bound retention | Roughly 69% smaller synthetic cache with the ordinary rowid candidate | Exact float payload round-trip, correct `emb_gen` handling, migration/restart/lookup checks |
| 4 | Reduce hidden-popup residency and evaluate graphics-resource choices | Target combined warm PSS at or below 100 MiB on this workload; result not yet proven | Memory and first-paint checks on supported desktops and real GPU drivers |
| 5 | Attribute daemon allocations and reduce oversized buffers or redundant refreshes | Additional RAM/CPU savings only where profiles justify them | Preserve query latency, allocations contract and update lag at 50k/500k entries |

### Step 1 details

Make release installation use the packaging path consistently, including future
development updates. Keep debug symbols separately for diagnostics. Adopt the
tested section flags in release recipes and inspect link maps for accidentally
retained modules. A later source/library split can make the client dependency
boundary explicit. Keep offline CLI indexing/query support and portable linkage.
Do not trade away matcher implementations or CPU compatibility just to shrink a
file. Compare on the same compiler/dependency versions before recording the
final package-size gate.

### Steps 2 and 3 details

Add a read-only storage report and an explicit maintenance operation through the
storage API; all application SQL remains in `src/storage/store.c`. Distinguish
catalog/history, reconstructible embeddings, free pages and managed model files.
Purging the cache must not delete `catalog.db`, roots, file identities or history.
Only offer model-file removal when it is managed by Torchlight and unreferenced.

Coordinate compaction under the existing database singleton and writer
ownership. Quiesce readers/writers for replacement, preserve the old database
until the replacement has passed integrity and identity checks, and handle WAL
sidecars correctly. Budget temporary disk space and test disk-full and interrupted
maintenance. [SQLite compaction can require substantial temporary space](https://www.sqlite.org/lang_vacuum.html);
do not put it on the normal query or startup path.

For retained caches, start with an ordinary rowid table and a unique
`(emb_gen,text)` key, preserving float payloads and current invalidation rules.
Evaluate the 8 KiB alternative separately: it affects database-wide page size
and has WAL migration implications. Consider a separately managed cache database
only if lifecycle and storage measurements justify that additional complexity.
Record the selected schema/retention design in an ADR before implementing it.

Use byte/age limits and explicit disabled-feature retention policy so cache
growth is bounded. Include re-enabling semantics and expensive cache rebuilds
in that policy. Removing an unused on-disk model/cache does **not** save the
equivalent RAM: neither is loaded by the current lexical-only daemon.

For this approximately 38k-entry workload, a **25–35 MiB total owned footprint**
is a reasonable target after smaller binaries, cache cleanup, compaction and
optional-model removal. This is an estimate, not a measured live cleanup. Keeping
semantic search and its model requires a separate, larger storage budget.

### Step 4 details

Measure cold start, warm show, repeated queries, hidden state and 100
show/search/hide cycles. Attribute heap, graphics mappings, shared libraries and
VRAM separately. Compare the current renderer with alternatives in controlled
tests; [GTK documents renderer overrides as debugging controls](https://docs.gtk.org/gtk4/running.html),
so do not blindly force an environment setting for all users.

Evaluate releasing previews/icons, unrealizing the hidden window, or exiting
after a measured idle delay while keeping the daemon resident. Choose according
to RAM saved versus reopening latency. Before any idle-exit policy, ensure
applications launched by Torchlight have an independent lifecycle: the observed
Spotify processes inherited the popup's service cgroup and must not be killed
when the launcher is restarted or exits. This also fixes misleading accounting.

### Step 5 details

Use heap/allocation profiles on synthetic 50k/500k catalogs with software
discovery enabled and disabled. Attribute file lexical indexes, desktop index,
software index, workspaces, watcher and ledger separately. Inspect the transient
overlap between old/new software catalogs and high-water allocations during
rebuilds. Current software discovery checks stamps every 15 seconds and performs
a backstop rebuild every 10 minutes; measure helper CPU and unchanged refreshes
before altering those intervals or publication policy.

## Validation and rollout

Keep this effort as separate changes: release linking/packaging, storage
maintenance, cache schema/retention, popup residency, then justified daemon work.
Run builds, experiments and automated checks in Boat. Retain native GPU checks
as a separate acceptance step because a cloud software renderer cannot establish
NVIDIA savings or compatibility.

For implementation, require:

- `make test`, `make lint`, package-install checks and relevant desktop actions.
- Unchanged local search ordering, all ten local slots, browser-default behavior,
  app installation handoff, offline CLI and optional semantic correctness.
- Storage migration and maintenance tests covering IDs/history, raw-byte paths,
  cache reuse, cancellation, interruption and insufficient disk space.
- Before/after PSS, private/anonymous memory, startup/rebuild peaks, process CPU
  including helpers, and owned disk size; exclude launched third-party apps.
- No more than 5 MiB settled growth after 100 popup cycles. Provisional warm
  combined-PSS goal: 100 MiB for the measured workload. Establish a cold/warm
  first-paint baseline before choosing a popup idle timeout.
- `make bench` and daemon benchmarks whenever query/indexing behavior changes;
  investigate regressions beyond run-to-run variability rather than accepting
  lower memory at the expense of search responsiveness.

Raw aggregate measurements, synthetic scripts/results, prototype build logs and
native-check results from this investigation are retained locally under
`build/footprint-audit/` (ignored by Git). The investigation used no private
database copy and did not modify the live installation. Subsequent approved
implementation and cleanup are recorded separately in the
[implementation record](footprint-implementation.md).
