# Ordinary app discovery — validation

2026-10-30, follow-up to [ADR 0070](adr/0070-app-discovery-without-a-prefix.md).
Builds and tests ran in Boat `bx_wfe7x4wh` (Ubuntu 24.04), not on the workstation.
The unchanged vendored Frizbee static library was reused from the preceding
verified build. No dependency or database schema changed.

## Behavior verified

- Ordinary app names show Install without a prefix. Enter hands the selected
  app ID to Software; typing alone performs no package command or activation.
- A native desktop entry with a different ID from the Flatpak app suppresses
  install suggestions and opens directly with Enter.
- Exact local filenames retain selection over an exact available-app name.
  Ten local slots remain available, with independent Web and suggestion slots.
- Unicode normalization, distinct app IDs, installed source variants, bounded
  results and cancellation are covered by unit and daemon contract tests.
- Semantic responses own a frozen copy of suggestions; mutating the caller's
  request after submission cannot change the final response.
- The new popup works with the preceding v2 daemon without sending the unknown
  suggestions field. Explicit Web/software modes and local-prefix escape work.
- Compact and wide GTK checks cover automatic installation, native app opening,
  direct websites, pinned Web, saved browser preferences and failed/stale actions.
  The existing theme/font/scale and AT-SPI matrix passes. Native rendering checks
  cover Open/Install/Manage labels and preview actions.

`make test` passed with ASan/UBSan and leak detection. `make lint` passed with
zero unsuppressed diagnostics. The optimized build passed the daemon contracts
and the wide-screen keyboard action checks. The allocation interposer
also verifies that repeated automatic suggestion queries allocate nothing.
Boat's inherited DAC capability was dropped with `setpriv` for the ordinary-user
permissions tests. Native rendering uses Xvfb and software GL; the virtual
display's DRI3 warnings do not imply a GTK diagnostic.
One GL animation sample missed the intermediate frame while compilation/static
analysis was active; its final idle-VM rerun passed in both Cairo and GL.

## Installed service

The verified portable archive was installed in `~/.local` and the daemon/popup
restarted. Live read-only queries confirmed a healthy index and a ready catalog
of 3,528 available apps. `vlc` returns the native `vlc.desktop` launcher with no
install suggestion. `Audacity` returns its available app with preferred Install
selection while retaining all ten local slots. `google.com` has no software
suggestions. Google/Chrome preferences and the lexical-only service override
were preserved byte-for-byte. The former binaries are backed up locally.

Portable archive SHA-256:
`d73447402f545a3e6abf37d22f75d29ae2d16bf5460214bb8827ee50d566b1f5`.

## Performance

`make bench`, `make bench-software` and the ordinary-query IPC comparison
completed on the idle VM. Times below are milliseconds; p95 is the 95th
percentile. These synthetic measurements are not desktop input-to-paint times.

| Measurement | p50 | p95 |
|---|---:|---:|
| Local lexical typing, 50k paths | 0.029 | 0.350 |
| Local lexical typing, 500k paths | 0.508 | 3.590 |
| Explicit software query, synthetic 50k-app cap | 3.127 | 11.142 |
| Automatic suggestions, synthetic 50k-app cap | 3.358 | 10.086 |

The existing 5 ms software-query target at the synthetic cap remains unmet.
This follow-up does not claim to close that M9 performance item.

With 50k local paths and the three-app adapter fixture, two enabled runs measured
ordinary-query engine p95 of 0.419–0.438 ms and IPC p95 of 0.500–0.520 ms.
Disabled runs measured 0.440–0.544 ms and 0.531–0.673 ms respectively. A blocked
software refresh left enabled engine p95 at 0.396–0.438 ms. These ranges show
run variability, not evidence that enabling suggestions speeds up local search.
Idle CPU was 0.2% of one core. The separate 50k-app stress benchmark above
measures catalog scale; the IPC fixture isolates orchestration.

## Scope

The install handoff uses inert software-manager fixtures in this follow-up;
real GNOME Software/Discover installation was checked during
[M9 validation](m9-completion.md). Torchlight still delegates final confirmation,
source selection and progress to the manager. No app was installed on the
workstation as a side effect of these tests.

Native package identity is not unified with Flatpak. Ordinary search prefers an
installed desktop launcher when its name matches; apps without such a launcher
can remain undetected, and explicit software mode can offer another format.
The bounded shortlist may omit less relevant alternatives.

Raw logs, captures, source snapshot, previous-install backup and the portable
build are retained in `build/app-discovery/` (ignored by Git).
