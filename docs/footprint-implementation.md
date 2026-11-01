# Footprint implementation — 2026-11-01

This implements the approved [footprint plan](footprint-plan.md), starting from
`20f4c6c`. All compilation, automated checks and synthetic profiling ran in Boat.
Only installation, explicitly requested cache/model removal and passive process
observations used the workstation. No private catalog was uploaded.

## Changes

Release builds use section garbage collection, and source installs strip the
release executables. Debug builds and unstripped release outputs remain available
for diagnostics. No dependency, query algorithm or ranking rule was added or
removed to achieve the binary reduction.

`torchlight storage` reads an existing v4/v5 catalog without migration.
`torchlight maintenance --purge-semantic` requires the daemon to be stopped,
transactionally removes only semantic data, and compacts through SQLite's
journaled VACUUM. It preserves file IDs, raw paths, roots, history and
`catalog_gen`. Purge and compaction are separate transactions: a failed
compaction can be retried after the purge has committed. The command does not
delete external model files, whose use by another catalog cannot be inferred.

Schema v5 copies retained vectors into an ordinary rowid table with the same
unique `(emb_gen,text)` key. Payload bytes, model descriptors and touched flags
are preserved. Existing activation prunes other generations and untouched texts;
disabled semantics retain reusable vectors until explicit cleanup. An arbitrary
age/byte eviction policy was not added: it would change reuse and inference cost
without helping this lexical-only installation. See
[ADR 0072](adr/0072-smaller-builds-and-storage-maintenance.md).

Dismissal clears the preview and unrealizes the popup window, releasing its
renderer and native surface while retaining the application/widgets. The
executable supplies a glibc `malloc_trim(0)` callback after 500 ms hidden idle;
quick reopening cancels it. No renderer environment override or automatic
process exit is installed. See
[ADR 0073](adr/0073-release-hidden-popup-surfaces.md).

## Live storage cleanup

The semantic model was unused (`emb_gen: null`) before and after cleanup.
With the daemon stopped, a temporary database backup and exact protected-row
snapshots were taken. The tested maintenance binary removed the semantic cache
directly from v4, avoiding an unnecessary copy into the new layout first.
Integrity checks passed and every protected row matched afterward: 37,980 file
entries, registered roots, 11 file-open events and 74 desktop-open events.
The temporary full backup was removed after verification and a healthy restart.

| Component | Before | After cleanup |
|---|---:|---:|
| Catalog database | 555.03 MiB | 10.31 MiB |
| Unused optional model | 62.32 MiB | 0 |
| Cached semantic records | 109,415 | 0 |

The unused managed `potion-retrieval-32M-256.tlm` file was explicitly removed.
The user service no longer names it; optional semantics require installing a
model and explicitly configuring `--model` again. The lexical-only setting,
system-default browser behavior and available-app discovery remain enabled as
before. WAL size varies with normal indexing activity.

The final stripped binaries were installed atomically into `~/.local/bin` and
the daemon restarted. It migrated to v5 successfully, passed an integrity check,
and retained every registered-root and history row from the pre-install backup.
The running daemon reports healthy indexing, 6,816 watches and 3,528 available
apps with semantics disabled. The popup was reopened from the new executable.

Installed binaries, data and fonts now occupy **17.96 MiB**, plus small desktop
integration/configuration files: approximately **18 MiB**, versus about 655 MiB
in the baseline (**about 97% less**). This excludes the development checkout,
build artifacts and shared system libraries. A full 4 MiB WAL would bring the
observed total closer to 22 MiB; it is normal for this total to vary.

A passive native-desktop sample after opening and hiding the new popup measured
daemon **50.13 MiB PSS** and popup **61.07 MiB PSS**, or **111.20 MiB combined**.
Immediately before replacement the old processes measured 52.96 + 78.77 =
131.73 MiB; the earlier investigation sample was 135.9 MiB. These observations
are not a controlled warm-workload experiment: restart state and shared pages
change PSS. The provisional **100 MiB combined target remains unmet**.

Spotify children previously charged to the popup service were moved into their
own transient scope before replacement and remained running. The replacement
popup service uses `ExitType=cgroup`, retaining launched children if the primary
popup process exits. The product itself has no automatic idle-exit policy.

## Daemon allocations and latency

Synthetic profiles used 50k/500k requested paths (49,469/494,262 indexed entries),
an empty watched root, and either no available-software catalog or 4,001 fake
Flatpak apps. They isolate resident indexes from real filesystem watches and
desktop history. Heap profiles and ordinary PSS runs were separate: instrumentation
changes allocator behavior, so allocated heap peaks are not resident RAM figures.

| Requested paths | Software apps | Resident PSS | CPU, daemon plus waited children |
|---|---:|---:|---:|
| 50,000 | 0 | 36.73 MiB | 0.25% of one core |
| 50,000 | 4,001 | 52.98 MiB | 1.31% of one core |
| 500,000 | 0 | 271.19 MiB | 0.25% of one core |
| 500,000 | 4,001 | 287.34 MiB | 1.13% of one core |

CPU observations span 16 seconds, including the 15-second software stamp check;
they do not establish long-term refresh cost. Full live watch coverage consumes
additional memory. Massif's useful allocated-heap peak at 500k was 451.09 MiB
during file-index construction, before software publication:

| Allocation group | MiB at that peak |
|---|---:|
| Prefix index | 170.35 |
| File-search columns | 106.78 |
| Trigram index | 64.00 |
| Typo index | 48.00 |
| Persistence ledger | 15.08 |
| Other / below attribution threshold | 46.88 |

Existing construction already shrinks search columns and releases scratch
buffers. Further reductions require index representation work with a latency
tradeoff; no speculative daemon algorithm change was made.

Three paired release-O2 runs at roughly 494k entries, with 1,200 held-out queries
per run, yielded engine p95 of **3.58–4.08 ms** for section-GC builds and
**3.07–4.84 ms** for original builds. The ranges overlap; there was no consistent
slowdown. This is a variability check, not evidence of a precise speedup.

## Validation and deployment record

`make test` and `make lint` passed on the final source inside an unprivileged
Boat-hosted container, with no compiler/linter warnings. Packaging checks passed,
and `make install` was staged with `DESTDIR` to inspect the actual stripped
files. The three installed executables total **7,529,056 bytes (7.18 MiB)**,
compared with **32.98 MiB** before: a **78.2% reduction**.

The release-build native action suite passed all ten local slots, Web placement,
address handling, default-browser changes during a session, ignored legacy
browser overrides, Software handoff, source revalidation, exact-file priority
and separation of typed actions from local launch history. Native view checks
passed with Cairo and GL.

Storage regressions cover exact migration bytes and model descriptors, key
uniqueness, raw-byte paths, IDs/roots/history preservation, read-only reporting,
singleton and SQL-reader conflicts, transaction rollback, file-size-limited
write failures, SIGKILL during WAL compaction, integrity after reopening, repeat
cleanup, and optional semantic reuse through the existing semantic suite.

GNOME, KDE and Cinnamon Wayland suites passed without skips, including native
view captures and window switching/reopening. The X11 suite initially retained
an obsolete assertion that reopening must preserve the X11 window ID. It now
requires a new native window belonging to the original application PID, matching
the intended resource-release behavior. Its complete rerun passed: actions,
theme/scaling/panel/GL cases, wide-window accessibility, keyboard launches,
restart/Retry, history, geometry and executable-update pickup, with no skips.

The desktop images ran sequentially in separate unprivileged containers on one
Boat VM. They use the repository's required desktop suites, private buses and
displays, dropped capabilities and disabled container networking. This run does
not claim validation on physical Wayland/GPU hardware.

### Popup memory

The 100-warm-up + 100-measured-cycle rapid-toggle checks used a 300 ms hidden
interval. That cancels the 500 ms allocator hook on most cycles; samples allow
one full second hidden for settling.

| Renderer | Hidden PSS after warm-up | After 100 more cycles | Growth | Warm first-paint p95 |
|---|---:|---:|---:|---:|
| GL / software Mesa | 107.16 MiB | 112.45 MiB | **5.29 MiB** | 102.43 ms |
| Cairo | 73.42 MiB | 73.72 MiB | **0.29 MiB** | 12.92 ms |

Cairo passes the 5 MiB growth gate. Rapid GL reopening misses it by 0.29 MiB;
this limit remains explicit rather than being rounded into a pass.

The matching GL comparison used one second hidden on every cycle, allowing the
new allocator hook to execute:

| Build | Hidden PSS after warm-up | After 100 more cycles | Growth | Warm first-paint p95 |
|---|---:|---:|---:|---:|
| Original | 251.10 MiB | 293.53 MiB | 42.43 MiB | 200.03 ms |
| Updated | 116.75 MiB | 122.31 MiB | **5.57 MiB** | 119.12 ms |

The updated popup retains about 58% less hidden PSS at the final sample and has
a lower warm-paint tail, but **GL still fails the 5 MiB growth gate**, including
with ordinary idle time. That remaining growth requires further allocation
attribution; the timing experiment alone does not establish its cause. No
renderer environment setting or idle-exit policy was used to hide the failure.
Renderer timing is cloud-specific, and Cairo also omits the GPU shadow path;
these measurements do not justify forcing Cairo for all users.

Aggregate results, native captures, source checksums and build/test logs are
retained under `build/footprint-implementation/` (ignored by Git). The temporary
rollback copies were removed after verifying the live installation; the Boat VM
was deleted after downloading its artifacts. Functional acceptance is complete;
the two RAM targets above remain open.

Software-rendered cloud checks cannot establish NVIDIA driver memory behavior;
native observations and cloud stress measurements are reported separately.

Reproduce profiling inside Boat after building release programs and fixtures:

```sh
python3 tests/bench/profile_footprint.py --massif --output build/heap-profile
python3 tests/run_popup_checks.py --memory --renderer ngl --screen 1280x800x24 \
    --output build/popup-memory-ngl.json
python3 tests/run_popup_checks.py --memory --renderer cairo --screen 1280x800x24 \
    --output build/popup-memory-cairo.json
```

The memory benchmark warms the entire 100-query workload before measuring
another 100 cycles. This separates first-use font/text/shader caches from growth
when repeating the same work. It requires settled hidden PSS growth of at most
5 MiB, and records cold/warm first-paint latency separately.
