"""End-to-end CLI/catalog fixtures including raw bytes and failed refreshes."""
import os
import shutil
import sqlite3
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
# Never read the developer's real configuration or catalog.
isolated = tempfile.mkdtemp(prefix="torchlight-cli-env-")
base_env = dict(os.environ, XDG_CONFIG_HOME=isolated, XDG_DATA_HOME=isolated)

def run_full(*args, success=True, env=None):
    result = subprocess.run([binary, *args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env or base_env, check=False)
    assert (result.returncode == 0) == success, result.stderr.decode(errors="replace")
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
    return result

def run(*args, success=True, env=None):
    return run_full(*args, success=success, env=env).stdout

def catalog_snapshot(database):
    """Include IDs, metadata, roots and catalog_gen when checking rollback."""
    with sqlite3.connect(database) as connection:
        return tuple(connection.iterdump())

def unavailable_root_aliases(temporary):
    """An unresolved spelling must never turn a configured root into deletion."""
    for number, spelling in enumerate(["a/", "a/./", "a/../a", "alias/"]):
        home = Path(temporary) / f"unavailable-{number}"
        (home / "a").mkdir(parents=True)
        (home / "b").mkdir()
        (home / "a/saved.txt").write_bytes(b"saved")
        (home / "b/other.txt").write_bytes(b"other")
        (home / "alias").symlink_to(home / "a", target_is_directory=True)
        database, config = home / "catalog.db", home / "config"
        config.write_text(f"root = {home}/{spelling}\nroot = {home}/b\n")
        arguments = ("index", "--db", str(database), "--config", str(config))
        run(*arguments)
        saved = run("query", "--db", str(database), "--null", "saved")
        assert saved == os.fsencode(home / "a/saved.txt") + b"\0"
        (home / "a").rename(home / ".offline-a")
        result = run_full(*arguments)
        assert b"root unavailable" in result.stderr
        assert run("query", "--db", str(database), "--null", "saved") == saved
        assert os.fsencode(home / "a") + b"\0" in run(
            "query", "--db", str(database), "--null", "")
        # A scanned ancestor must also preserve an unresolved symlink's target.
        config.write_text(f"root = {home}/{spelling}\nroot = {home}\n")
        run(*arguments)
        assert run("query", "--db", str(database), "--null", "saved") == saved
        # Deferred removals finish once the configured root resolves again.
        (home / ".offline-a").rename(home / "a")
        config.write_text(f"root = {home}/{spelling}\n")
        run(*arguments)
        assert run("query", "--db", str(database), "--null", "other.txt") == b""

def storage_failure_rolls_back(temporary):
    """A callback TL_IO must fail the whole refresh, even with another good root."""
    home = Path(temporary) / "storage-failure"
    for name in ["a", "b"]:
        (home / name).mkdir(parents=True)
        (home / name / "old.txt").write_bytes(b"old")
    database, config = home / "catalog.db", home / "config"
    config.write_text(f"root = {home}/a\nroot = {home}/b\n")
    arguments = ("index", "--db", str(database), "--config", str(config))
    run(*arguments)
    with sqlite3.connect(database) as connection:
        # Fixture-only SQL injects a write failure; production SQL stays in store.c.
        connection.execute("CREATE TRIGGER fail_test_write BEFORE INSERT ON files "
                           "WHEN NEW.name = X'626c6f636b65642e747874' "
                           "BEGIN SELECT RAISE(FAIL, 'injected write failure'); END")
    before = catalog_snapshot(database)
    (home / "a/blocked.txt").write_bytes(b"fail")
    (home / "b/new.txt").write_bytes(b"new")
    (home / "a/old.txt").write_bytes(b"changed metadata")
    # Exercise failures both before and after a healthy root has been written.
    for roots in [("a", "b"), ("b", "a")]:
        config.write_text("".join(f"root = {home}/{root}\n" for root in roots))
        result = run_full(*arguments, success=False)
        assert b"root unavailable" not in result.stderr
        assert catalog_snapshot(database) == before
    with sqlite3.connect(database) as connection:
        connection.execute("DROP TRIGGER fail_test_write")
    run(*arguments)
    assert run("query", "--db", str(database), "new.txt") != b""

def config_sync(temporary):
    """`index` without roots syncs the catalog to the configuration file."""
    home = Path(temporary) / "sync"
    for directory in ["a/sub", "a/.secret", "a/.config/nvim", "b", "c"]:
        (home / directory).mkdir(parents=True)
    for file in ["a/one.txt", "a/sub/two.txt", "a/.secret/hidden.txt",
                 "a/.config/nvim/init.lua", "b/three.txt", "c/four.txt"]:
        (home / file).write_bytes(b"")
    database, config = home / "catalog.db", home / "config"
    def catalog():
        return set(run("query", "--db", str(database), "--null", "--limit", "1000",
                       "/").split(b"\0")[:-1])
    def index():
        return run_full("index", "--db", str(database), "--config", str(config)).stderr
    # Duplicate and covered roots are scanned once; allowlisted hidden dirs are indexed.
    config.write_text(f"root = {home}/a\nroot = {home}/a/sub\nroot = {home}/a/\n"
                      f"# comment\nallow = {home}/a/.config/nvim\nroot = {home}/b\n")
    index()
    paths = catalog()
    assert os.fsencode(home / "a/.config/nvim/init.lua") in paths
    assert os.fsencode(home / "a/.secret/hidden.txt") not in paths
    assert os.fsencode(home / "a/.config") not in paths
    assert os.fsencode(home / "b/three.txt") in paths
    assert len([p for p in paths if p.endswith(b"two.txt")]) == 1
    # An unavailable configured root keeps the run going and keeps saved entries.
    config.write_text(f"root = {home}/a\nroot = {home}/b\nroot = {home}/missing\n")
    assert b"root unavailable" in index()
    assert os.fsencode(home / "b/three.txt") in catalog()
    # Removing a root from the configuration forgets it and its entries.
    config.write_text(f"root = {home}/a\nroot = {home}/c\n")
    assert b"Forgot" in index()
    paths = catalog()
    assert os.fsencode(home / "b/three.txt") not in paths
    assert os.fsencode(home / "c/four.txt") in paths
    assert os.fsencode(home / "a/.config/nvim/init.lua") not in paths  # no longer allowed
    # Malformed and unknown entries fail with a message and change nothing.
    before = catalog()
    config.write_text("root = /x\nthis line is wrong\n")
    result = run_full("index", "--db", str(database), "--config", str(config), success=False)
    assert b"line 2" in result.stderr
    config.write_text("mystery = 1\n")
    result = run_full("index", "--db", str(database), "--config", str(config), success=False)
    assert b"mystery" in result.stderr
    assert catalog() == before

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
    env = dict(base_env, XDG_DATA_HOME=str(root / "state" / ".." / "state"))
    run("index", str(root), env=env)
    state_db = root / "state" / "torchlight" / "catalog.db"
    assert b"/state/torchlight" not in run("query", "--db", str(state_db), "--null", "--limit", "1000", "/")
    config_sync(temporary)
    unavailable_root_aliases(temporary)
    storage_failure_rolls_back(temporary)
    for invalid in ["0", "-1", "1001", "1x", "999999999999999999999"]:
        run("query", "--db", str(database), "--limit", invalid, "x", success=False)
    run("query", "--db", str(database), "x" * 257, success=False)
shutil.rmtree(isolated)
print("CLI integration tests passed.")
