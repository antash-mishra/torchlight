# 0068. Required desktop tests in Docker images on parallel Boat workers

- **Status:** Accepted
- **Date:** 2026-10-30

## Context

Torchlight has isolated X11, GNOME and other Wayland desktop checks, but running
them depends on what happens to be installed. Missing desktops or accessibility
bindings currently skip successfully. The user requested multiple environment
images and parallel testing on their cloud subscription before M9 development.
The launcher-comparison runner already demonstrates Docker images and Boat forks.

## Decision

Use one shared Ubuntu 24.04/Rust 1.89 build image and four desktop layers: X11,
GNOME Shell, KWin and Cinnamon. Run the baseline tests and lint once, snapshot
the prepared VM, then fork one VM per selected desktop. Default concurrency is
four, configurable downward. Export images into the home filesystem before the
snapshot and restore/verify them on workers: a live fork test showed that newly
built images were unavailable when Docker reported ready, despite dormant
container references. Actual desktop sessions run in unprivileged Docker
containers with private displays, buses and fixtures. All execution happens in
Boat; local orchestration only transfers files and collects reports.
Export BuildKit caches into the home filesystem separately from runnable images,
because Boat excludes Docker's internal build cache from snapshots and restored
image tags did not supply cache to its BuildKit backend.
After successful baseline checks, prune unused Docker images from the disposable
base; workers restore the explicit archive instead of competing with background
restoration of redundant images. Exported images and build caches remain intact.
Put temporary fixtures on a bounded private tmpfs. A live sanitizer run timed
out while rebuilding 10,000 semantic rows on Docker's writable layer; the
isolated case took 15.2 seconds there and 0.8 seconds on tmpfs. Keep the existing
test deadlines and treat these jobs as functional checks, not storage benchmarks.

Required matrix jobs fail when a desktop or accessibility dependency is missing,
while optional developer checks may still skip. Preserve diagnostics on failure,
record native view images, source provenance and package versions, and report
each job independently. Include working-tree edits and untracked source files
in uploads. Journal allocated sandbox ids and delete only those owned by the run,
including after failure. Cleanup failures also fail the overall run.

See [Desktop tests on Boat](../desktop-test-matrix.md) for commands and coverage.

## Alternatives considered

- One machine running every desktop at once would share CPU, session services
  and build paths. Separate workers give each job its own resources and make
  failures easier to reproduce.
- A full distribution-by-desktop Cartesian matrix would require much more
  maintenance. Establish four desktop sessions first, then add distribution
  and full login/installation tests where they give additional coverage.
- Treating missing tools as passing would conceal broken test images. Required
  jobs must establish that the intended checks actually ran.
- Sharing the host desktop or installing dependencies on the host would violate
  the repository's execution boundary and risk tests changing the working session.

## Consequences

Desktop dependencies and startup recipes live in the repository, and independent
jobs can run concurrently. The first matrix covers Ubuntu userspace across four
desktop environments. Kernel, real GPU/monitor, full login and software-manager
authentication behavior need additional VM or hardware tests. Screenshots are
diagnostics alongside behavioral assertions, not the sole acceptance criterion.
Benchmarks remain separate from this functional matrix.
