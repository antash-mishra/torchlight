"""Required desktop checks and retained diagnostics for isolated acceptance tests."""
from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def unavailable(reason):
    """Fail a required environment, or report an optional local check as skipped."""
    required = os.environ.get("TORCHLIGHT_TEST_REQUIRED") == "1"
    print(f"{'Required desktop check failed' if required else 'Desktop check skipped'}: {reason}",
          flush=True)
    if required:
        raise SystemExit(1)


@contextmanager
def session_directory(prefix):
    """Yield a private fixture directory; retain regular diagnostics even on failure.

    TORCHLIGHT_TEST_ARTIFACTS opts into copying logs, measurements and images.
    Runtime sockets, symlinks and fixture databases never enter the report.
    """
    with tempfile.TemporaryDirectory(prefix=prefix) as temporary:
        try:
            yield temporary
        finally:
            destination = os.environ.get("TORCHLIGHT_TEST_ARTIFACTS")
            if destination:
                root = Path(destination)
                root.mkdir(parents=True, exist_ok=True)
                saved = Path(tempfile.mkdtemp(prefix=prefix, dir=root))
                for source in Path(temporary).rglob("*"):
                    if (source.is_symlink() or not source.is_file() or
                            source.suffix not in (".log", ".json", ".jsonl", ".png")):
                        continue
                    target = saved / source.relative_to(temporary)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)


def capture_view(environment, activate=None):
    """Run native view assertions and captures; optionally activate its windows while running."""
    destination = os.environ.get("TORCHLIGHT_TEST_VIEW_CAPTURE")
    if not destination:
        return
    Path(destination).mkdir(parents=True, exist_ok=True)
    env = dict(environment, ASAN_OPTIONS="detect_leaks=0", GSK_RENDERER="cairo",
               G_DEBUG="fatal-warnings")
    command = ["build/test_popup_view"]
    deadline = time.monotonic() + 120
    with subprocess.Popen(command, env=env) as process:
        try:
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(command, 120)
                if activate is not None:
                    activate()
                time.sleep(0.05)
            if process.returncode:
                raise subprocess.CalledProcessError(process.returncode, command)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
