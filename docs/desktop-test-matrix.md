# Desktop tests on Boat

The desktop matrix builds Torchlight once, then runs four required desktop jobs
on separate Boat VMs. Each job uses a Docker image with its own compositor,
session bus, unprivileged test user and fixture data. The launcher comparison
images in `tests/bench/compare/` remain a separate benchmark.

## Run it

From the repository root, with an authenticated `boat` CLI:

```sh
make test-desktop-matrix
# Fewer concurrent VMs, or just the environments relevant to a change:
make test-desktop-matrix MATRIX_ARGS='--workers 2 --lanes x11 gnome'
```

The local machine needs Python 3, Git and Boat. It only archives source, issues
Boat commands, collects artifacts and writes the report. Docker, compilation,
unit/integration tests, lint and desktop checks all run in Boat, as required by
`AGENTS.md`. No Docker daemon or desktop dependencies are needed on the host.

Allow tens of minutes for a cold run: it compiles the offline Rust matcher and
installs four desktops before running checks. Desktop jobs reuse that compiled
image. The template option below can also reuse build inputs across runs.

The default is four concurrent desktop workers after the base build. The base
is stopped before workers are forked. Every sandbox is created without account
secrets, with a three-hour TTL as a backstop, and explicitly deleted at the end,
including on test failure or interruption. Existing account sandboxes are never
selected or deleted. Check `boat limits` for current account capacity and credit.
Desktop workers use `--no-snapshots`: stopping one erases it. Only a
snapshot-enabled base can be kept as a reusable template.

For repeated development runs, `--template SANDBOX_ID` can fork a stopped
sandbox containing the exported matrix images and BuildKit caches. The template
itself is never stopped or deleted by the runner. Its new fork receives a fresh checkout and
reports. Explicit filesystem caches are reused only when Docker's build inputs
match. A template containing images alone still needs a cold build.

## Coverage

| Job | Image/session | Required checks |
|---|---|---|
| Base | Ubuntu 24.04, Rust 1.89.0, GTK 4.14+ | Debug build and native fixtures, `make test`, `make lint` |
| X11 | Xvfb and Metacity | Idle exit and child survival, Web/software keyboard actions, theme/font/scale matrix, local keyboard/mouse actions, window children, wide preview and AT-SPI |
| GNOME | GNOME Shell's headless Wayland session | Enable the extension, list/switch windows, real popup plus native GTK view assertions |
| KDE | KWin's virtual Wayland session | KWin scripts, list/switch windows, real popup plus native GTK view assertions |
| Cinnamon | Cinnamon Wayland nested on Xvfb | Enable the extension, list/switch windows, repeated showing and native GTK view assertions |

These initial images share Ubuntu's userspace to isolate desktop differences.
Cinnamon on Ubuntu is not full Linux Mint coverage; KWin is not a complete KDE
login session. Fedora/Arch userspace and actual distribution boot/login checks
can be added separately. Containers share the VM's kernel. Software rendering
does not validate physical GPUs, physical monitors or real fractional scaling.

## What the runner does

1. Archives the current working tree's tracked and unignored files, including
   uncommitted edits and new test files. Ignored build output and `.git` are
   excluded. A source hash and Git commit identify what was actually tested.
2. Creates a fresh base VM and builds the shared image, then each selected
   desktop layer. Runs sanitizer tests and lint as the ordinary container user.
3. Exports all images with `docker save` into
   `/home/user/torchlight-matrix-images/`. After the baseline passes it removes
   unused Docker images from this disposable base, then stops it and waits for its
   disk snapshot to finish. The archive is shared across forks through that
   snapshot. It stays in the cloud rather than being downloaded with reports.
4. Forks one VM per desktop job and starts all jobs in a wave before waiting.
   `--workers` limits each wave to one through four concurrent jobs. Each worker
   restores images with `docker load` when needed and verifies their identities
   against the build manifest. Tests run without network access or elevated
   container capabilities; images are never implicitly pulled.
5. Collects each result independently. A failed job does not suppress the other
   reports. Deletes the run's sandboxes and returns nonzero for any failed check,
   missing report, required skip or cleanup failure.

The VM's `vm.mmap_rnd_bits` is set to 28 for sanitizer compatibility, matching
the release workflow. This happens only in the disposable cloud VM. The Docker
containers use an init process, private shared memory and bounded execution
timeouts. Fixture files use a private 1 GiB `/tmp` tmpfs; executable fixture
scripts require its `exec` option. This keeps functional test deadlines from
depending on Docker's writable-layer I/O latency. It does not measure disk
durability or storage performance. No host display, D-Bus socket, Docker socket or home directory is
mounted into them.

Desktop jobs also start a private system bus: GNOME needs a connection even in a
headless session. It exposes no host services. KWin's packaged realtime file
capability is removed in its image so the virtual backend can execute with all
container capabilities dropped. These sessions do not test privileged system
services or login/authorization flows.

The GNOME fixture creates a virtual keyboard through
[Mutter 46's remote-input interface](https://github.com/GNOME/mutter/blob/46.2/data/dbus-interfaces/org.gnome.Mutter.RemoteDesktop.xml)
and activates native fixture windows through Torchlight's extension. A headless
Mutter session otherwise has no keyboard focus for GTK's typing checks. This is
private compositor API, so a GNOME upgrade must revalidate that setup.

## Reports and debugging

Each invocation creates `build/desktop-matrix/<UTC timestamp>/`. A custom
`--output` path must not already exist, so reports cannot overwrite earlier
results. Files include:

- `report.md` and `report.json`: job results and explicit passed/skipped counts.
- `source.json` and `source.tar.gz`: provenance and the exact uploaded source.
- `build/`: image build logs, image ids and the test/lint baseline report.
- `<desktop>/result.json`, `versions.log`: individual check results and actual
  installed package/toolchain versions.
- `<desktop>/<check>/output.log`, `sessions/`, `screenshots/`: retained stdout,
  compositor/popup diagnostics and native GTK fixture PNGs from that compositor.
- `sandboxes.json`, `cleanup.json`: the run's allocated ids and any deletion
  requests that failed after retries. An empty cleanup list means no failures.

Screenshots document the native view fixtures; window-list and activation checks
use assertions against the running desktop and accessibility tree. Passing does
not depend on a screenshot looking plausible. Base/toolchain images are pinned
by digest, while distribution package updates can change; versions and image ids are
recorded rather than claiming bit-for-bit package reproducibility.

If the orchestrator itself is forcibly killed or its machine loses power, use
the ids in `sandboxes.json` to inspect/delete its sandboxes. A report names any
cleanup failures rather than returning success with workers left running.
The remote-only `worker.py` can rebuild images in a disposable development VM;
it is not a host test command.

The first live fork check showed that Boat preserved the checkout and reports,
but newly built images were unavailable when Docker reported ready, even with
dormant containers referring to them. Explicit image archives avoid relying on
Docker-store restore timing or retention behavior.
Removing unused images after export also avoids competing with Boat's background
image restoration: an earlier worker import took several minutes alongside it.
Restoration failures and mismatched image identities fail the worker.
Boat also [excludes the ordinary Docker build cache from snapshots](https://docs.boat.dev/snapshots/whats-saved.md#docker-builds).
Builds therefore export a [local BuildKit cache](https://docs.docker.com/build/cache/backends/local/)
per image under `/home/user/torchlight-matrix-images/build-cache/`, replacing the
previous export after a successful build. This requires the containerd image
store used by Boat's Docker setup. Loaded images alone did not restore build
cache in a live fork check; image-tag cache imports attempted a registry pull.

## Required tests and diagnostics

`TORCHLIGHT_TEST_REQUIRED=1` turns missing compositor or Python accessibility
bindings into failures. Ordinary developer invocations retain optional skips.
The matrix also rejects a successful process that prints a skipped-check marker.

`TORCHLIGHT_TEST_ARTIFACTS=/path` retains regular `.log`, `.json`, `.jsonl` and
`.png` diagnostics on both success and failure. It does not follow symlinks or
copy sockets/databases. `TORCHLIGHT_TEST_VIEW_CAPTURE=/path` enables native view
captures and assertions within the Wayland test session. Without those settings,
the tests keep their existing temporary-directory cleanup behavior.

`tests/test_environment_runner.py`, included in `make test`, covers missing
requirements, false success from skipped tests, process-group timeout cleanup,
failure diagnostics, dirty source transfer and cloud cleanup ownership.
It also checks the private system bus lifecycle, portable image restoration and
retrying a transient failure while polling a running cloud job.
The collector requires all four X11 groups; regression coverage rejects a
missing group and accepts the complete idle, Web/software, matrix and AT-SPI run.
The bus lifecycle check is optional on ordinary headless developer environments
without D-Bus tools; matrix jobs require those tools and execute the check.
The clean parallel build also exposed a missing Frizbee prerequisite on
`build/test_window_sources`; it is now declared and covered by a make-graph
regression that requires the matcher build before the fixture's link command.
The native header-width assertion runs inside the styled popup, so the check
measures the application's font rather than the desktop's unstyled default.
The fixture also uses the launcher's compositor-dependent surface handling,
including Cinnamon's hide/show workaround (ADR 0054).
X11 compares the search anchor after opening finishes; comparing an animated
empty frame against a settled result can otherwise report a fractional-pixel
movement. GNOME waits for Shell's startup notice before launching Torchlight,
so its initial settings restoration cannot race with extension enablement.

Keep performance benchmarks on quiet workers. This matrix establishes functional
compatibility, not comparable latency while several workloads share a VM.

## Verification

The 2026-11-01 idle-popup change passed four X11 groups (idle lifecycle,
Web/software actions, themes/scales and wide AT-SPI) plus GNOME, KDE and Cinnamon
Wayland: seven groups, zero skips, all with native view captures. The desktop
lanes ran sequentially on a separate Boat VM from the controlled performance
comparison. Unit/sanitizer tests and lint passed on the comparison VM; all 19
runner regressions passed again after updating the collector to require four
X11 groups. Artifacts are in `build/idle-comparison/idle-matrix/`; see the
[idle-popup comparison](idle-popup-comparison.md) for exact measurements and
the separate real-systemd and release lifecycle checks.

The M9 run adds typed Web/software previews and a third X11 group for browser
and software-manager activation. An existing writer-repair assertion raced the
completion counters after catalog publication; it now waits for the completed
pass before checking scoped-repair statistics. See [M9 validation](m9-completion.md)
for the feature run and its separate real-manager checks.

The earlier five-group baseline below predates M9. M9 has six desktop groups:
three X11 groups and one each for GNOME, KDE and Cinnamon.
All six passed with zero skips. The final cold X11 rebuild also passed
`make test` (including 19 runner regressions) and `make lint`; its artifact root
is `build/desktop-matrix/20261030-m9-x11-final/`. GNOME, KDE and Cinnamon passed
in `20261030-m9-acceptance/` before the final local-error-message and collector
fixes. The original failed X11 run remains there for diagnosis. Both runs'
cleanup lists are empty. See `build/desktop-matrix/20261030-m9-verification.md`
for the combined index and [M9 validation](m9-completion.md) for exact scope.

Verified on 2026-10-30 with Ubuntu 24.04, GTK 4.14.5 and Rust 1.89.0:

| Check | Result |
|---|---|
| `make test` and `make lint` | Passed |
| Runner regressions | 18 passed, including the final worker changes |
| X11: Metacity 3.52.0 / Xvfb 21.1.12 | Theme/font/scale matrix and wide AT-SPI checks passed |
| GNOME Shell 46.0 / Mutter 46.2 | Window integration and native view checks passed |
| KWin 5.27.11 | Window integration and native view checks passed |
| Cinnamon 6.0.4 | Window integration, repeated showing and native view checks passed |
| Exported build cache on a fresh fork | Rust compilation restored as `CACHED`; current-source test/lint passed |

The five standalone Dockerfiles built in a cold cloud run. Final desktop
acceptance refreshed current sources in retained environment images and started
four separate workers before collecting results. X11 and GNOME exposed the
fixture timing issues described above; those two jobs were fixed and rerun in
parallel. The latest passing results combine that rerun with the unchanged KDE
and Cinnamon checks. All five desktop check groups executed with zero skips and
produced native view screenshots.

Local artifacts are under `build/desktop-matrix/20261030-desktop-final/`,
`20261030-fixture-fixes/` and `20261030-cache-check/`. Source archives, package
versions, image identities, earlier failures and subsequent passing results are
retained separately; `build/desktop-matrix/20261030-verification.md` summarizes
the latest results and confirms no task sandboxes remain. The sanitizer fixture timeout was diagnosed with an
isolated comparison: 15.2 seconds on the writable image layer and 0.8 seconds on
tmpfs. Test deadlines were kept intact. Functional timing here is not a storage
or launcher performance benchmark.
