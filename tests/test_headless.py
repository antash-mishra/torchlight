"""Headless make targets never need GTK or X11 (ADR 0037).

Each case dry-runs make with a pkg-config stand-in that records every query
and fails GTK/X11 lookups, as on a machine without those libraries. Recipes
are only printed, so this checks what the targets would run, not a build.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

make = sys.argv[1] if len(sys.argv) > 1 else "make"
source = Path(__file__).resolve().parent.parent
GUI_PACKAGES = ("gtk4", "x11")
# The parent make's flags (jobserver descriptors, -n) must not leak in.
base_env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")}

def dry_run(temporary, *targets):
    """Return (printed recipes, pkg-config queries) for a forced dry run."""
    log = temporary / "queries.log"
    log.write_text("")
    probe = temporary / "pkg-config"
    probe.write_text(
        "#!/bin/sh\n"
        f'printf "%s\\n" "$*" >> "{log}"\n'
        'case "$*" in *gtk4*|*x11*) exit 1;; esac\n'
        "exit 0\n")
    probe.chmod(0o755)
    result = subprocess.run(
        [make, "-C", str(source), "--dry-run", "--always-make", f"PKG_CONFIG={probe}",
         f"DESTDIR={temporary / 'stage'}", *targets],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=base_env, check=False)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    return result.stdout.decode(errors="replace"), log.read_text().splitlines()

def gui_queries(queries):
    return [query for query in queries if any(package in query for package in GUI_PACKAGES)]

def headless_targets_skip_the_popup(temporary):
    """Build, install and lint the daemon and CLI without touching GTK."""
    output, queries = dry_run(temporary, "headless", "install-headless", "lint-headless")
    assert gui_queries(queries) == [], gui_queries(queries)
    assert "build/torchlightd " in output and "build/torchlight " in output
    assert "torchlight-gtk" not in output and "ui/gtk/" not in output
    assert "org.torchlight.Launcher.desktop" not in output
    # The headless unit replaces the graphical one under the same name.
    assert "packaging/torchlightd-headless.service" in output
    assert "lib/systemd/user/torchlightd.service" in output

def full_build_still_uses_gtk(temporary):
    """Control case: the probe does see the popup's GTK lookups."""
    output, queries = dry_run(temporary, "all", "install")
    assert gui_queries(queries) != []
    assert "src/bin/torchlight-gtk.c" in output
    assert "org.torchlight.Launcher.desktop" in output

temporary = Path(tempfile.mkdtemp(prefix="torchlight-headless-"))
try:
    headless_targets_skip_the_popup(temporary)
    full_build_still_uses_gtk(temporary)
finally:
    shutil.rmtree(temporary)
print("Headless build tests passed.")
