"""Storage reporting, v4 migration and offline maintenance failure regressions."""
import fcntl
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import resource
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())


@contextmanager
def connect(path):
    """sqlite3's own context commits but does not close its connection."""
    db = sqlite3.connect(path)
    try:
        with db:
            yield db
    finally:
        db.close()


def run(env, *args, success=True, preexec_fn=None):
    result = subprocess.run([binary, *map(str, args)], env=env, capture_output=True,
                            preexec_fn=preexec_fn, timeout=30)
    assert (result.returncode == 0) == success, result.stderr.decode(errors="replace")
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
    return result


def protected(db):
    """All persisted user content, excluding deliberately removable semantic columns."""
    queries = ["SELECT id,path,name,ext,is_dir,mtime,size,identity FROM files ORDER BY id",
               "SELECT * FROM roots ORDER BY path", "SELECT * FROM searches ORDER BY id",
               "SELECT * FROM opens ORDER BY event_id",
               "SELECT * FROM desktop_opens ORDER BY event_id",
               "SELECT * FROM sqlite_sequence ORDER BY name",
               "SELECT * FROM meta WHERE key!='schema_version' ORDER BY key"]
    return tuple(tuple(db.execute(query)) for query in queries)


def vectors(db):
    return tuple(db.execute("SELECT emb_gen,text,payload,touched FROM embedding_cache "
                            "ORDER BY emb_gen,text"))


def fixture(base, env, count=4000):
    root = base / "files"
    root.mkdir()
    (root / "saved.txt").write_text("saved")
    raw = os.fsencode(root) + b"/raw-\xff.txt"
    with open(raw, "wb"):
        pass
    path = base / "catalog.db"
    run(env, "index", "--db", path, root)
    with connect(path) as db:
        db.execute("DROP TABLE embedding_cache")
        db.execute("CREATE TABLE embedding_cache(emb_gen TEXT NOT NULL REFERENCES "
                   "embedding_models(emb_gen) ON DELETE CASCADE,text BLOB NOT NULL,"
                   "payload BLOB NOT NULL,touched INTEGER NOT NULL DEFAULT 1,"
                   "PRIMARY KEY(emb_gen,text)) WITHOUT ROWID")
        db.execute("PRAGMA user_version=4")
        db.execute("UPDATE meta SET value='4' WHERE key='schema_version'")
        db.execute("INSERT INTO embedding_models VALUES('7','descriptor',1)")
        db.execute("INSERT INTO embedding_models VALUES('8','staging descriptor',0)")
        # Include zeros/non-text bytes; migration must copy payloads exactly.
        payload = bytes(range(256)) * 4
        db.executemany("INSERT INTO embedding_cache VALUES(?,?,?,?)",
                       ((str(7 + i % 2), f"text-{i:08}".encode(), payload, i % 2)
                        for i in range(count)))
        file_id = db.execute("SELECT id FROM files WHERE path=?", (raw,)).fetchone()[0]
        db.execute("UPDATE files SET emb_version='legacy',emb_bin=X'00FF',"
                   "emb_i8=X'7F80',emb_scale=0.25 WHERE id=?", (file_id,))
        db.execute("INSERT INTO searches VALUES('search','saved',1)")
        db.execute("INSERT INTO opens VALUES('open',?,'search',1)", (file_id,))
        db.execute("INSERT INTO desktop_opens VALUES('desktop','example.desktop','search',1)")
        db.commit()
        db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    return path


def integrity(path, before):
    with connect(path) as db:
        assert db.execute("PRAGMA integrity_check").fetchall() == [("ok",)]
        assert db.execute("PRAGMA foreign_key_check").fetchall() == []
        assert protected(db) == before


def migration(base, env):
    path = fixture(base, env)
    with connect(path) as db:
        before, cached = protected(db), vectors(db)
        models = db.execute("SELECT * FROM embedding_models ORDER BY emb_gen").fetchall()
    digest = hashlib.sha256(path.read_bytes()).digest()
    report = run(env, "storage", "--db", path).stdout
    assert b"Cached embeddings: 4000\n" in report
    assert hashlib.sha256(path.read_bytes()).digest() == digest  # no migration/report writes
    run(env, "query", "--db", path, "saved")
    integrity(path, before)
    with connect(path) as db:
        assert db.execute("PRAGMA user_version").fetchone() == (5,)
        assert db.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone() == ("5",)
        assert vectors(db) == cached
        assert db.execute("SELECT * FROM embedding_models ORDER BY emb_gen").fetchall() == models
        schema = db.execute("SELECT sql FROM sqlite_master WHERE name='embedding_cache'").fetchone()[0]
        assert "WITHOUT ROWID" not in schema.upper()
        try:
            db.execute("INSERT INTO embedding_cache SELECT * FROM embedding_cache LIMIT 1")
            raise AssertionError("duplicate cache key was accepted")
        except sqlite3.IntegrityError:
            pass
    run(env, "maintenance", "--db", path)
    integrity(path, before)
    with connect(path) as db:
        assert vectors(db) == cached  # compaction alone retains semantic data


def cleanup(base, env):
    path = fixture(base, env)
    with connect(path) as db:
        before = protected(db)
    old_size = path.stat().st_size
    run(env, "maintenance", "--db", path, "--purge-semantic")
    assert path.stat().st_size < old_size / 10
    integrity(path, before)
    with connect(path) as db:
        assert db.execute("PRAGMA user_version").fetchone() == (4,)  # no needless migration
        assert db.execute("SELECT count(*) FROM embedding_cache").fetchone() == (0,)
        assert db.execute("SELECT count(*) FROM embedding_models").fetchone() == (0,)
        assert db.execute("SELECT count(*) FROM files WHERE emb_version IS NOT NULL OR "
                          "emb_bin IS NOT NULL OR emb_i8 IS NOT NULL OR emb_scale IS NOT NULL").fetchone() == (0,)
        assert db.execute("PRAGMA freelist_count").fetchone() == (0,)
    run(env, "maintenance", "--db", path, "--purge-semantic")
    integrity(path, before)
    assert run(env, "query", "--db", path, "saved").stdout


def conflicts_and_failures(base, env):
    path = fixture(base, env)
    with connect(path) as db:
        before, cached = protected(db), vectors(db)
    with open(str(path) + ".daemon.lock", "a") as lock:
        os.chmod(lock.name, 0o600)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert b"stop torchlightd" in run(env, "maintenance", "--db", path,
                                        "--purge-semantic", success=False).stderr
        run(env, "storage", "--db", path)  # reporting is safe with a live singleton
    with connect(path) as reader:
        reader.execute("BEGIN")
        reader.execute("SELECT * FROM files").fetchall()
        run(env, "maintenance", "--db", path, "--purge-semantic", success=False)
        reader.rollback()
    with connect(path) as db:
        db.execute("CREATE TRIGGER refuse_cleanup BEFORE DELETE ON embedding_models "
                   "BEGIN SELECT RAISE(ABORT,'injected purge failure'); END")
    run(env, "maintenance", "--db", path, "--purge-semantic", success=False)
    with connect(path) as db:
        assert vectors(db) == cached  # cache deletion rolled back with model deletion
        db.execute("DROP TRIGGER refuse_cleanup")
        # Prevent ALTER's destination from being created; schema migration must roll back.
        db.execute("CREATE TABLE embedding_cache_v4(blocked INTEGER)")
    run(env, "query", "--db", path, "saved", success=False)
    with connect(path) as db:
        assert db.execute("PRAGMA user_version").fetchone() == (4,)
        assert vectors(db) == cached
        db.execute("DROP TABLE embedding_cache_v4")
    def limit_files():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (65536, 65536))
    run(env, "query", "--db", path, "saved", success=False, preexec_fn=limit_files)
    with connect(path) as db:
        assert db.execute("PRAGMA user_version").fetchone() == (4,)
        assert vectors(db) == cached
    run(env, "maintenance", "--db", path, success=False, preexec_fn=limit_files)
    integrity(path, before)
    with connect(path) as db:
        assert vectors(db) == cached
    run(env, "maintenance", "--db", path, "--purge-semantic")
    integrity(path, before)


def interrupted_compaction(base, env):
    path = fixture(base, env, count=12000)
    with connect(path) as db:
        before, cached = protected(db), vectors(db)
        db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    wal = Path(str(path) + "-wal")
    with subprocess.Popen([binary, "maintenance", "--db", str(path)], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
        deadline = time.monotonic() + 20
        while child.poll() is None and time.monotonic() < deadline:
            if wal.exists() and wal.stat().st_size > 65536:
                child.kill()
                break
            time.sleep(.001)
        output, errors = child.communicate(timeout=10)
        assert child.returncode == -signal.SIGKILL, (output, errors, child.returncode)
    integrity(path, before)
    with connect(path) as db:
        assert vectors(db) == cached
    run(env, "maintenance", "--db", path, "--purge-semantic")
    integrity(path, before)


with tempfile.TemporaryDirectory(prefix="torchlight-storage-") as temporary:
    root = Path(temporary)
    env = dict(os.environ, XDG_CONFIG_HOME=str(root), XDG_DATA_HOME=str(root))
    for test in (migration, cleanup, conflicts_and_failures, interrupted_compaction):
        case = root / test.__name__
        case.mkdir()
        test(case, env)
    missing = root / "missing.db"
    for command in ("storage", "maintenance"):
        run(env, command, "--db", missing, success=False)
        assert not missing.exists()
    run(env, "storage", "--purge-semantic", success=False)
print("Storage migration, maintenance, contention and failure tests passed.")
