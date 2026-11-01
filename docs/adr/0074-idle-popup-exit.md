# 0074: Exit the popup after a minute hidden

Status: Accepted (2026-11-01)

After surface release and allocator trimming, the workstation still attributes
about 65 MiB PSS to the hidden popup, including about 41 MiB in graphics mappings.
The daemon uses about 55 MiB. GTK and driver lifetime, rather than the stripped
executable, dominate the popup's remaining background footprint.

Keep the popup warm for 60 seconds after dismissal, then end its process.
The daemon remains resident. The existing desktop shortcut invokes the executable
on every press, so a later press starts a fresh single-instance popup. Reopening
cancels pending exit, including an asynchronous service-policy check. Pending
launch completion and open-history IPC postpone exit. Visible windows never
expire. `--idle-exit-seconds=0` opts into permanent residency; 1..86400 configures
the delay. No renderer or search/ranking changes accompany this policy.

A user service can terminate launched applications or restart the popup on exit.
Read `/proc/self/cgroup`; scopes and unmanaged processes can exit normally.
For a service, query the user systemd manager on a worker. A non-main process
may exit unless the service follows a D-Bus name. A main process needs cgroup
lifetime and a policy that does not restart on success; a watchdog or forced
restart exit codes make it stay resident. Unknown/unreachable service policy
also retains the process. `KillMode=process` alone is insufficient because custom
stop hooks can kill children. The launcher does not mutate units or reparent apps.
See the [systemd service contract](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html#ExitType=).

This extends ADR 0073's surface release with an explicit process lifetime policy.
Its tradeoff is fresh-process startup after idle: font/style/driver initialization
runs again, while the search index remains ready. Measure quick and idle reopen
separately; compare equal workloads/renderers and preserve the resident option.
Cloud graphics timings do not establish native GPU timings. Growth during a
continuously warm session remains a separate metric; process exit must not be
used to disguise a failed warm-growth gate.

Validation and user-visible measurements are in the
[comparison record](../idle-popup-comparison.md).
