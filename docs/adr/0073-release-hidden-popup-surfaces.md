# 0073: Release hidden popup rendering resources

Status: Accepted, with remaining performance targets recorded (2026-11-01)

The installed popup retains roughly 78 MiB PSS, with a separate sample attributing
about 43 MiB to graphics-driver mappings. Hiding its window retains the renderer.
The existing fresh-surface path used for older Cinnamon Wayland sessions already
supports unrealizing the hidden window and realizing it at the next show.

Enable that path on every desktop after the dismissal animation. Keep the GTK
application, widgets and daemon alive. Clear the hidden preview to cancel its
file reads and release details/icons; detach old surface callbacks immediately
because drivers can retain a destroyed surface. Reopening creates a new native
surface and renderer, repositions the window, and uses the existing widgets.

The first GL comparison reduced 100-cycle growth from about 94 MiB to 6.9 MiB,
but freed renderer allocations still remained in glibc's arenas. The executable
therefore supplies a best-effort allocator-release callback, as the daemon does.
The launcher calls it once after a window has been unrealized and hidden for
500 ms; reopening or destruction cancels the timer. The library remains
allocator-neutral. On glibc the executable calls `malloc_trim(0)`; other C
libraries retain their normal policy. No allocator environment override ships.

Do not force a renderer or automatically exit the application. An idle-exit policy
would also require separating launched applications from the popup's service
lifecycle, as the observed Spotify processes demonstrate. Unloading driver libraries
is not guaranteed by unrealizing a window; measured results determine the benefit.

Acceptance covers immediate and animated dismissal, cancellation of a pending
dismissal, re-realization, native actions and compositor behavior. The native
frame probe follows each new frame clock. A reproducible 100-cycle benchmark
records visible/hidden PSS, growth and cold/warm paint latency for old/new builds.
Cloud graphics results are distinguished from the actual desktop's drivers.

The matching GL idle comparison reduced final hidden PSS from 293.53 to
122.31 MiB and warm-paint p95 from 200.03 to 119.12 ms. Settled GL growth remains
5.57 MiB per 100 measured cycles, above the 5 MiB target; the native desktop's
observed combined PSS is 111.20 MiB, above the provisional 100 MiB target. Cairo
growth passes at 0.29 MiB. These are measured improvements with open RAM targets,
not complete performance acceptance. See the
[implementation record](../footprint-implementation.md).
