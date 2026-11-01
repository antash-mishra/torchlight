# Idle popup comparison — 2026-11-01

Baseline: `0498752` (the installed build with the P100 label removed).
The candidate adds the policy in [ADR 0074](adr/0074-idle-popup-exit.md).
The tested candidate is installed on the workstation. All four desktop lanes
passed; measurements and validation scope are recorded below.

## What changes for the user

The shortcut, search results, keyboard actions and appearance stay the same.
After dismissal, the popup stays warm for about a minute. A shortcut during
that minute reuses it and cancels exit. After a longer break the popup process
ends, and the next shortcut starts it again. The daemon keeps its index and
continues watching files, so restarting the popup does not rebuild the index.

This trades a small fresh-process startup delay after idle for releasing all
of the popup process's background memory. RAM while open, or during the first
hidden minute, is approximately unchanged. Installed apps, browser visits and
the Software manager continue running independently of the popup's normal exit.

Custom service policies that could kill children or restart the popup make it
stay resident. Unknown service settings also retain it. The opt-out is
`torchlight-gtk --idle-exit-seconds=0 --toggle`; a value from 1 to 86400 changes
the delay. Ordinary desktop shortcuts continue to work without modification.
Add the option to a shortcut command to retain a custom delay on future launches.

The daemon binary is byte-for-byte identical between the two cloud builds
(SHA256 `f00cf0ca6a4fe81338d6d9986d082fc3339c036d55378987ea6133f59c5b097d`).
The app-install catalog, query algorithms and renderer policy are unchanged.
Catalog compaction and a lighter renderer remain separate proposals, with no
claimed savings in this change.

## Controlled popup measurements

Both release builds ran sequentially on the same Boat VM, using private Xvfb
displays and D-Bus sessions, Metacity, GTK's `ngl` renderer and software Mesa
with `LP_NUM_THREADS=2`. Two runs per build used the order baseline/candidate,
then candidate/baseline. Each run measured 20 fresh processes, 20 warm reopens
and 20 warm query paints, followed by a 65-second hidden wait and one reopen.
The fixture contains 100 files and no available-software or system-app catalog;
it isolates popup behavior and does not represent the workstation daemon's RAM.

The ranges below span the two runs. PSS accounts for shared pages proportionally.
Fresh-process samples use ordinary OS caching, without dropping filesystem or
driver caches; they do not measure a cold boot. p95 is the slowest observation
with this 20-sample percentile definition, so it is not a population guarantee.

| Metric | Baseline | Candidate |
|---|---:|---:|
| Popup shown PSS | 123.57–125.37 MiB | 123.53–124.27 MiB |
| Popup PSS after 1 second hidden | 94.78–96.76 MiB | 95.06–96.61 MiB |
| Popup PSS after 65 seconds hidden | 94.78–96.76 MiB | **0 MiB: process exited** |
| Quick reopen, median first paint | 70.87–72.20 ms | 69.68–71.08 ms |
| Quick reopen, p95 first paint | 77.72–79.66 ms | 74.65–78.71 ms |
| Warm typing to results, median | 39.68–41.20 ms | 41.32–45.66 ms |
| Warm typing to results, p95 | 54.42–56.42 ms | 50.50–54.81 ms |
| Fresh-process first paint, median | 114.50–119.43 ms | 111.97–112.24 ms |
| Fresh-process first paint, p95 | 125.13–260.73 ms | 120.14–127.70 ms |
| First paint after 65 seconds hidden | 72.16–75.57 ms | 113.77–116.18 ms |
| Observed idle-exit delay | Remains resident | 59.75–60.56 seconds |

The paired post-idle cost is **40.61–41.61 ms extra** in this environment.
Quick reopening and typing tails are comparable; the small sample and first-run
startup outlier do not establish a fresh-start speedup. These cloud timings are
not native NVIDIA latency measurements. The existing GL stress-growth target
is separate from idle release; a process exit must not count as a pass for
continuous warm-cycle growth.

## Native background RAM

The installed baseline's SHA256 exactly matches the stripped baseline used in
the cloud comparison. The candidate was installed atomically after sanitizer,
lint, native-view, action-survival and real-systemd checks passed. The daemon
was not replaced or restarted; the same daemon process continued throughout.

| Native observation | Before replacement | After candidate idle exit |
|---|---:|---:|
| Hidden popup PSS | 64.76 MiB | 0 MiB |
| Daemon PSS | 56.84 MiB | 55.56 MiB |
| Combined PSS | **121.60 MiB** | **55.56 MiB** |

The observed combined reduction is **66.04 MiB (54.3%)**. The daemon's 1.28 MiB
variation is not attributed to a popup-only change. Shortly after hiding, the
fresh candidate used 61.07 MiB PSS before its idle timeout; that brief-use sample
is not a controlled warm comparison against the older process's 64.76 MiB.
All popup PIDs were absent after idle, and the live daemon still reported healthy
indexing and 3,528 available apps. The existing Alt+Space shortcut points directly
to the updated `~/.local/bin/torchlight-gtk --toggle`. The independently running
Spotify scope remained active.

This meets a sub-100-MiB **idle** total on this workload. It does not meet the
previous sub-100-MiB **warm** total target; the first hidden minute and visible
use still retain the GTK process. Native observations establish background RAM,
not native first-paint latency. PSS is process attribution, not a measurement of
system-wide free-memory increase or GPU VRAM.

## Continuous warm use, search and executable size

An additional matching GL test warmed all 100 queries, then measured another
100 show/query/hide cycles. Each hide lasted one second, so neither build exited
during the workload. Both used the same cloud renderer/thread settings above.

| Warm-session metric | Baseline | Candidate |
|---|---:|---:|
| Hidden popup PSS after warm-up | 102.82 MiB | 100.78 MiB |
| Hidden popup PSS after another 100 cycles | 106.68 MiB | 105.44 MiB |
| Growth across those 100 cycles | 3.86 MiB | 4.66 MiB |
| Warm first-paint p95 | 96.73 ms | 87.43 ms |

Both pass the 5 MiB growth gate in this run. The candidate's observed growth is
0.79 MiB greater, so this is not evidence that warm growth has been fixed. The
earlier 5.57 MiB result in the footprint record used another cloud configuration;
these tests do not establish a universal graphics-memory bound.

The separate daemon comparison used 1,200 held-out queries per corpus and the
same release-O2 executable bytes. Small timing differences are run variability,
not an engine optimization:

| Daemon metric | Baseline | Candidate |
|---|---:|---:|
| Engine p95, 49,569 entries | 0.563 ms | 0.488 ms |
| IPC round-trip p95, 49,569 entries | 0.623 ms | 0.541 ms |
| Engine p95, 494,362 entries | 3.042 ms | 3.012 ms |
| IPC round-trip p95, 494,362 entries | 3.134 ms | 3.092 ms |
| File-update publication, 494,362 entries | 105.96 ms | 107.03 ms |
| Steady daemon RSS, 494,362 entries | 271,028 KiB | 271,032 KiB |
| Stripped popup executable | 552,032 bytes | 560,224 bytes |

The popup adds **8 KiB** on disk; no dependency, model or data migration is added.
No daemon CPU reduction is claimed. The benefit is ending the idle UI process;
reopening after idle repeats its startup CPU work. The previous passive
16-second native observation had about 0.31% of one core for the daemon and
0.00% for the hidden popup, so CPU was already low.

## Validation

Completed: `make test`, `make lint`, Cairo and GL native-view assertions,
the release typed-action and popup lifecycle checks, and real-systemd checks. Lifecycle coverage
includes visible retention, quick-reopen cancellation, shortcut presses on both
sides of the idle deadline, clean idle exit, restored
search, invalid timeout values, the resident option, and survival of an inert
application, browser, file opener and Software manager after popup exit.
Real services cover ordinary main-process lifetime, `Restart=always`, and
compatible cgroup lifetime; fixture children survive and restart counts stay zero.

The X11 matrix passed idle behavior, Web/software actions, themes/scales,
keyboard/mouse/window actions, wide preview and accessibility. GNOME, KDE and
Cinnamon Wayland checks also passed: seven required groups, zero skips, with
native view captures from each group. These are isolated cloud desktop sessions,
not physical GPU or full distribution login coverage. Production launcher and
lifecycle source hashes match across the workstation and both test VMs.

The matrix collector now requires the new fourth X11 group; all 19 runner
regressions passed again after updating that expectation. The final expanded
idle test separately passed shortcut presses around the timeout boundary.

## Reproduction and evidence

Run compilation and automated checks in Boat, as required by `AGENTS.md`.
Build each revision with the same release toolchain and retain the baseline:

```sh
python3 tests/run_popup_checks.py --idle-bench --baseline --renderer ngl \
  --launcher /path/to/baseline/torchlight-gtk --output baseline.json
python3 tests/run_popup_checks.py --idle-bench --renderer ngl \
  --launcher build/release/torchlight-gtk --output candidate.json
python3 tests/run_popup_checks.py --idle --launcher build/release/torchlight-gtk
```

`tests/test_popup_service.py` additionally uses a private display with the Boat
VM's real user systemd bus. It creates uniquely named test services, checks
child survival and restart counts, then stops only those fixture services.

Raw measurements and logs are retained under `build/idle-comparison/idle-evidence/`
and desktop results under `build/idle-comparison/idle-matrix/` (ignored by Git).
Both task-owned Boat sandboxes were deleted after collection; deletion completed
on 2026-11-01. No pre-existing sandbox was selected or removed.
The previous disk/cache changes and their measurements remain in the
[footprint implementation record](footprint-implementation.md).
