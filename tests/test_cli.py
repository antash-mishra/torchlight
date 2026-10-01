"""End-to-end CLI/catalog fixtures including raw bytes and failed refreshes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())

def run(*args, success=True, env=None):
    result = subprocess.run([binary, *args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, check=False)
    assert (result.returncode == 0) == success, result.stderr.decode(errors="replace")
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
    return result.stdout

with tempfile.TemporaryDirectory(prefix="torchlight-cli-") as temporary:
    root = Path(temporary) / "root"
    root.mkdir()
    database = Path(temporary) / "catalog.db"
    (root / "projectNotes.md").write_bytes(b"hello")
    (root / ".hidden-file").write_bytes(b"")
    for name in [".hidden", "node_modules", "target", "build", "__pycache__"]:
        (root / name).mkdir()
        (root / name / "not-indexed").write_bytes(b"")
    (root / "link").symlink_to(root, target_is_directory=True)
    raw = os.fsencode(root) + b"/bad\xff\n.md"
    fd = os.open(raw, os.O_WRONLY | os.O_CREAT, 0o600)
    os.close(fd)
    run("index", "--db", str(database), str(root))
    def catalog():
        return run("query", "--db", str(database), "--null", "--limit", "1000", "/")
    paths = set(catalog().split(b"\0")[:-1])
    assert len(paths) == 5, paths
    assert raw in paths and os.fsencode(root / "link") in paths
    assert run("query", "--db", str(database), "prjnts").strip().endswith(b"projectNotes.md")
    assert run("query", "--db", str(database), "--null", "bad") == raw + b"\0"
    assert b"\xef\xbf\xbd\\x0a" in run("query", "--db", str(database), "bad")
    assert run("query", "--db", str(database), "").strip() == os.fsencode(root)
    run("index", "--db", str(database), str(root))
    assert set(catalog().split(b"\0")[:-1]) == paths
    (root / "projectNotes.md").unlink()
    run("index", "--db", str(database), str(root))
    assert run("query", "--db", str(database), "prjnts") == b""
    before = catalog()
    run("index", "--db", str(database), str(root / "unavailable"), success=False)
    assert catalog() == before
    # Explicit hidden roots override default filtering.
    run("index", "--db", str(database), str(root / ".hidden"))
    assert b"not-indexed" in run("query", "--db", str(database), "not-indexed")
    # Regression: re-indexing the parent must not prune the nested hidden root.
    run("index", "--db", str(database), str(root))
    assert b".hidden/not-indexed" in run("query", "--db", str(database), "not-indexed")
    # Regression: an unreadable subdirectory failed the whole scan. The rest of
    # the root is now indexed while the unreadable scope keeps its saved entries.
    inaccessible = root / "unreadable"
    inaccessible.mkdir()
    (inaccessible / "saved").write_bytes(b"")
    run("index", "--db", str(database), str(root))
    if os.geteuid() != 0:
        inaccessible.chmod(0)
        try:
            (root / "fresh.txt").write_bytes(b"")
            run("index", "--db", str(database), str(root))
            assert b"saved" in run("query", "--db", str(database), "saved")
            assert b"fresh.txt" in run("query", "--db", str(database), "fresh")
        finally:
            inaccessible.chmod(0o700)
    # Intentionally excluded hidden directories remain exclusions when unreadable.
    hidden = root / ".hidden"
    if os.geteuid() != 0:
        hidden.chmod(0)
        try:
            run("index", "--db", str(database), str(root))
        finally:
            hidden.chmod(0o700)
    # Custom DB files/sidecars inside the root must not enter the catalog.
    inside_db = root / "inside.db"
    run("index", "--db", str(inside_db), str(root))
    inside_paths = run("query", "--db", str(inside_db), "--null", "--limit", "1000", "/")
    assert os.fsencode(inside_db) not in inside_paths
    # Defaults honor XDG and never include Torchlight's own state.
    env = dict(os.environ, XDG_DATA_HOME=str(root / "state" / ".." / "state"))
    run("index", str(root), env=env)
    state_db = root / "state" / "torchlight" / "catalog.db"
    assert b"/state/torchlight" not in run("query", "--db", str(state_db), "--null", "--limit", "1000", "/")
    for invalid in ["0", "-1", "1001", "1x", "999999999999999999999"]:
        run("query", "--db", str(database), "--limit", invalid, "x", success=False)
    run("query", "--db", str(database), "x" * 257, success=False)
print("CLI integration tests passed.")
