"""M2 acceptance: live byte paths, moves, history, bounded clients and recovery."""
import base64
from concurrent.futures import ThreadPoolExecutor
import itertools
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

cli = str(Path(sys.argv[1]).resolve())
binary = str(Path(sys.argv[2]).resolve())
sequence = itertools.count()


def wait_for(predicate, timeout=12):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(0.025)
    raise AssertionError(f"condition not reached; last value: {last!r}")


def exact_path(result):
    assert isinstance(result["id"], str) and int(result["id"]) > 0
    assert ("path" in result) != ("path_b64" in result)
    return result["path"].encode() if "path" in result else base64.b64decode(result["path_b64"], validate=True)


class Service:
    def __init__(self, base, *extra):
        self.base = Path(base)
        self.root = self.base / "root"
        self.root.mkdir(exist_ok=True)
        self.database = self.base / "catalog.db"
        self.config = self.base / "config"
        self.config.write_text(f"root = {self.root}\n")
        self.path = self.base / "torchlight.sock"
        self.extra = extra
        self.env = dict(os.environ, HOME=str(self.base), XDG_RUNTIME_DIR=str(self.base),
                        XDG_CONFIG_HOME=str(self.base / "configuration"),
                        XDG_DATA_HOME=str(self.base / "state"),
                        XDG_DATA_DIRS=str(self.base / "no-system-apps"))
        self.process = None
        self.log = None

    def start(self):
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([binary, "--db", str(self.database), "--config", str(self.config),
                                         "--rescan-ms", "500", *self.extra], env=self.env,
                                        stdout=self.log, stderr=self.log)
        def ready():
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(self.log.read().decode(errors="replace"))
            try:
                return self.call("status")
            except (FileNotFoundError, ConnectionRefusedError, OSError):
                return False
        wait_for(ready)
        return self

    def stop(self, crash=False):
        if self.process is None:
            return
        self.process.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
        self.process.wait(timeout=12)
        self.log.seek(0)
        log = self.log.read()
        self.log.close()
        assert b"AddressSanitizer" not in log and b"runtime error:" not in log, log
        assert self.process.returncode == (-signal.SIGKILL if crash else 0), log
        self.process = None
        if not crash:
            assert not self.path.exists()

    def connect(self):
        connection = socket.socket(socket.AF_UNIX)
        connection.settimeout(8)
        connection.connect(str(self.path))
        return connection

    def call(self, op, **fields):
        request = dict(version=1, request_id=str(next(sequence)), op=op, **fields)
        with self.connect() as connection:
            connection.sendall(json.dumps(request).encode() + b"\n")
            with connection.makefile("rb") as stream:
                line = stream.readline(1024 * 1024)
        response = json.loads(line)
        assert response["version"] == 1 and response["phase"] == "final"
        assert response["request_id"] == request["request_id"]
        assert "catalog_gen" in response and "indexing" in response and "emb_gen" in response
        return response

    def paths(self, query="/"):
        response = self.call("query", query=query, limit=1000)
        assert response["status"] == "ok", response
        results = response["results"]
        assert len({row["id"] for row in results}) == len(results)
        assert len({exact_path(row) for row in results}) == len(results)
        return {exact_path(row): row["id"] for row in results}

    def indexed(self, path):
        target = os.fsencode(path)
        return wait_for(lambda: self.paths().get(target))

    def command(self, *arguments, success=True):
        result = subprocess.run([cli, *arguments], env=self.env, capture_output=True, timeout=10)
        assert (result.returncode == 0) == success, result.stderr
        assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
        return result.stdout


def live_catalog(service):
    root = service.root
    (root / "projectNotes.md").write_text("hello")
    (root / "folder/sub").mkdir(parents=True)
    (root / "folder/sub/report.pdf").write_bytes(b"report")
    raw = os.fsencode(root) + b"/bad\xff\n.md"
    Path(root / "valid\nname.txt").write_bytes(b"")
    fd = os.open(raw, os.O_CREAT | os.O_WRONLY, 0o600)
    os.close(fd)
    (root / ".hidden").mkdir()
    (root / ".hidden/skipped").write_bytes(b"")
    service.start()
    try:
        original = service.indexed(root / "projectNotes.md")
        raw_id = service.indexed(raw)
        assert raw not in service.command("query", "bad")  # safe display, invalid byte replaced
        assert service.command("query", "--null", "bad") == raw + b"\0"
        raw_result = service.call("resolve", file_id=raw_id)["results"][0]
        assert exact_path(raw_result) == raw and "path_b64" in raw_result
        assert service.command("resolve", "--null", raw_id) == raw + b"\0"
        raw_renamed = os.fsencode(root) + b"/renamed\xfe\n.pdf"
        os.rename(raw, raw_renamed)
        assert service.indexed(raw_renamed) == raw_id
        assert exact_path(service.call("resolve", file_id=raw_id)["results"][0]) == raw_renamed
        assert b"valid\nname.txt\0" in service.command("query", "--null", "valid")
        assert b"skipped" not in service.command("query", "skipped")
        # Both singleton locks are exercised, including a database alias.
        for database, path in [(service.database, service.base / "other.sock"),
                               (service.base / "./catalog.db", service.base / "other.sock")]:
            duplicate = subprocess.run([binary, "--db", str(database), "--config", str(service.config),
                                        "--socket", str(path)], env=service.env, capture_output=True, timeout=8)
            assert duplicate.returncode != 0
        assert service.command("index", "--db", str(service.database), str(root), success=False) == b""
        assert (service.path.stat().st_mode & 0o777) == 0o600
        assert service.call("query", query="prjnts")["results"][0]["id"] == original

        created = root / "created.txt"
        created.write_bytes(b"created")
        created_id = service.indexed(created)
        renamed = root / "renamed.txt"
        created.rename(renamed)
        assert service.indexed(renamed) == created_id
        assert service.call("resolve", file_id=created_id)["results"][0]["path"] == str(renamed)
        renamed.unlink()
        wait_for(lambda: service.call("resolve", file_id=created_id)["reason"] == "stale_result")
        renamed.write_bytes(b"replacement")
        assert service.indexed(renamed) != created_id
        # A rename over an existing file keeps the source id and retires the
        # overwritten destination id, including path embedding metadata.
        source, target = root / "source.md", root / "target.pdf"
        source.write_bytes(b"source")
        target.write_bytes(b"target")
        source_id, target_id = service.indexed(source), service.indexed(target)
        with sqlite3.connect(service.database) as connection:
            connection.execute("UPDATE files SET emb_version='fixture',emb_bin=X'01' WHERE id=?", (int(source_id),))
        source.rename(target)
        wait_for(lambda: service.paths().get(os.fsencode(target)) == source_id)
        assert service.call("resolve", file_id=target_id)["reason"] == "stale_result"
        with sqlite3.connect(service.database) as connection:
            assert connection.execute("SELECT typeof(path),name,ext,emb_version,emb_bin FROM files WHERE id=?",
                                      (int(source_id),)).fetchone() == ("blob", b"target.pdf", b"pdf", None, None)
        child_id = service.indexed(root / "folder/sub/report.pdf")
        folder_id = service.indexed(root / "folder")
        (root / "folder").rename(root / "moved")
        assert service.indexed(root / "moved/sub/report.pdf") == child_id
        assert service.indexed(root / "moved") == folder_id
        # A newly created descendant under a moved watch must be found too.
        (root / "moved/sub/fresh.txt").write_bytes(b"")
        service.indexed(root / "moved/sub/fresh.txt")

        # An external SQLite write lock must never block resident queries.
        with sqlite3.connect(service.database, timeout=3) as connection:
            connection.execute("BEGIN IMMEDIATE")
            for _ in range(12):
                assert service.call("query", query="projectNotes.md")["results"][0]["id"] == original
            connection.rollback()

        # Concurrent clients and filesystem updates may observe either complete
        # snapshot; every envelope/result stays valid and contains unique ids.
        def queries():
            for _ in range(30):
                assert os.fsencode(root / "projectNotes.md") in service.paths()
        with ThreadPoolExecutor(max_workers=4) as pool:
            futures = [pool.submit(queries) for _ in range(4)]
            for i in range(20):
                (root / f"burst-{i}.txt").write_bytes(b"")
            service.indexed(root / "burst-19.txt")
            for future in futures:
                future.result()

        # Accepted history is asynchronous, ordered and deduplicated.
        query = service.call("query", query="projectNotes.md")
        event = dict(file_id=original, search_id=query["search_id"], event_id="accepted-open")
        assert service.call("open", **event)["status"] == "ok"
        assert service.call("open", **event)["status"] == "ok"
        def recorded():
            with sqlite3.connect(service.database) as connection:
                rows = connection.execute("SELECT file_id,search_id FROM opens WHERE event_id='accepted-open'").fetchall()
                return rows == [(int(original), query["search_id"])]
        wait_for(recorded)
        stale = service.call("open", file_id=created_id, event_id="stale-open")
        assert stale["reason"] == "stale_result"
        assert service.call("history_clear")["status"] == "ok"
        def cleared():
            with sqlite3.connect(service.database) as connection:
                return connection.execute("SELECT count(*) FROM opens").fetchone()[0] == 0
        wait_for(cleared)

        # Unreadable/offline scopes retain saved rows, and recover later.
        if os.geteuid() != 0:
            folder = root / "moved"
            folder.chmod(0)
            try:
                assert service.call("reconcile")["status"] == "ok"
                start = service.call("status")["indexing"]["reconciliations"]
                wait_for(lambda: service.call("status")["indexing"]["reconciliations"] > start)
                assert service.paths()[os.fsencode(folder / "sub/report.pdf")] == child_id
            finally:
                folder.chmod(0o700)
        # Failure rolls back files and catalog_gen and leaves the resident view.
        rollback_source = root / "rename-rollback.txt"
        rollback_target = root / "rename-recovered.txt"
        rollback_source.touch()
        rollback_id = service.indexed(rollback_source)
        with sqlite3.connect(service.database) as connection:
            connection.execute("CREATE TRIGGER fail_m2 BEFORE INSERT ON files WHEN NEW.name=X'626c6f636b65642e747874' BEGIN SELECT RAISE(FAIL,'injected failure'); END")
            before = connection.execute("SELECT value FROM meta WHERE key='catalog_gen'").fetchone()[0]
        (root / "blocked.txt").write_bytes(b"")
        rollback_source.rename(rollback_target)
        wait_for(lambda: service.call("status")["indexing"]["degraded"])
        with sqlite3.connect(service.database) as connection:
            assert connection.execute("SELECT value FROM meta WHERE key='catalog_gen'").fetchone()[0] == before
            assert connection.execute("SELECT count(*) FROM files WHERE name=X'626c6f636b65642e747874'").fetchone()[0] == 0
            connection.execute("DROP TRIGGER fail_m2")
        assert service.paths()[os.fsencode(root / "projectNotes.md")] == original
        service.indexed(root / "blocked.txt")
        assert service.indexed(rollback_target) == rollback_id

        # Malformed framing and duplicate active ids are rejected per connection.
        for invalid in [b'{"version":2,"request_id":"bad","op":"status"}\n',
                        b'{"version":1,"request_id":"bad","op":"query","query":"\\u0000"}\n',
                        b'{"version":1,"request_id":"bad","op":"query","query":"x","query":"y"}\n']:
            with service.connect() as connection:
                connection.sendall(invalid)
                with connection.makefile("rb") as stream:
                    assert json.loads(stream.readline())["status"] == "error"
        with service.connect() as connection:
            request = b'{"version":1,"request_id":"same","op":"status"}\n'
            connection.sendall(request + request)
            with connection.makefile("rb") as stream:
                assert json.loads(stream.readline())["status"] == "ok"
                assert json.loads(stream.readline())["reason"] == "duplicate_active_request_id"
        with service.connect() as connection:
            first = dict(version=1, request_id="old", op="query", query="project")
            last = dict(version=1, request_id="new", op="query", query="projectNotes.md")
            connection.sendall(json.dumps(first).encode() + b"\n" + json.dumps(last).encode() + b"\n")
            with connection.makefile("rb") as stream:
                assert json.loads(stream.readline())["status"] == "cancelled"
                assert json.loads(stream.readline())["results"][0]["id"] == original
        with service.connect() as connection:
            connection.sendall(b'{"version":1,"request_id":"half-close","op":"status"}\n')
            connection.shutdown(socket.SHUT_WR)
            with connection.makefile("rb") as stream:
                assert json.loads(stream.readline())["status"] == "ok"
        # A stalled input client expires while other clients remain responsive.
        with service.connect() as stalled:
            stalled.sendall(b'{"version":')
            for _ in range(12):
                assert service.call("status")["status"] == "ok"
            time.sleep(5.2)
            assert stalled.recv(1) == b""
        with service.connect() as oversized:
            oversized.sendall(b"x" * 8192)
            assert oversized.recv(1) == b""

        # Crash keeps committed state. Start serves it while root is unavailable;
        # only later successful reconciliation can remove a disappeared file.
        service.stop(crash=True)
        root.rename(service.base / "offline")
        service.start()
        assert service.paths()[os.fsencode(root / "projectNotes.md")] == original
        (service.base / "offline").rename(root)
        (root / "projectNotes.md").unlink()
        wait_for(lambda: service.call("resolve", file_id=original)["reason"] == "stale_result")
    finally:
        service.stop()


def watch_exhaustion_and_disabled_history(service):
    (service.root / "sub").mkdir()
    (service.root / "sub/saved.txt").write_bytes(b"")
    service.start()
    try:
        service.indexed(service.root / "sub/saved.txt")
        wait_for(lambda: service.call("status")["indexing"]["watch_degraded"])
        (service.root / "sub/unwatched.txt").write_bytes(b"")
        file_id = service.indexed(service.root / "sub/unwatched.txt")
        assert service.call("open", file_id=file_id, event_id="disabled")["status"] == "ok"
        time.sleep(0.1)
        with sqlite3.connect(service.database) as connection:
            assert connection.execute("SELECT count(*) FROM searches").fetchone()[0] == 0
            assert connection.execute("SELECT count(*) FROM opens").fetchone()[0] == 0
        assert not service.call("status")["history"]["enabled"]
    finally:
        service.stop()


def rapid_replacements(service):
    """A coalesced replacement retires stale selections and rolls back on failure."""
    file = service.root / "same.txt"
    folder = service.root / "tree"
    child = folder / "child.txt"
    file.write_bytes(b"old")
    folder.mkdir()
    child.write_bytes(b"old")
    service.start()
    try:
        original, folder_id, child_id = (service.indexed(path) for path in (file, folder, child))
        query = service.call("query", query="same.txt")
        assert service.call("open", file_id=original, search_id=query["search_id"],
                            event_id="replaced-open")["status"] == "ok"
        def history_recorded():
            with sqlite3.connect(service.database) as connection:
                return connection.execute("SELECT count(*) FROM opens WHERE event_id='replaced-open'").fetchone()[0] == 1
        wait_for(history_recorded)
        # Keep the old inode allocated, making replacement deterministic even
        # on filesystems that cannot report a birth timestamp.
        with file.open("rb"), sqlite3.connect(service.database) as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute("CREATE TRIGGER fail_replace BEFORE INSERT ON files WHEN NEW.name=X'73616d652e747874' BEGIN SELECT RAISE(FAIL,'injected replacement failure'); END")
            connection.commit()
            connection.execute("BEGIN IMMEDIATE")
            before = connection.execute("SELECT value FROM meta WHERE key='catalog_gen'").fetchone()[0]
            file.unlink()
            file.write_bytes(b"replacement")
            connection.rollback()
            wait_for(lambda: service.call("status")["indexing"]["degraded"])
            assert service.paths()[os.fsencode(file)] == original
            assert connection.execute("SELECT value FROM meta WHERE key='catalog_gen'").fetchone()[0] == before
            assert connection.execute("SELECT count(*) FROM opens WHERE event_id='replaced-open'").fetchone()[0] == 1
            connection.execute("DROP TRIGGER fail_replace")
            connection.commit()
            wait_for(lambda: service.paths().get(os.fsencode(file)) != original)
        assert service.call("resolve", file_id=original)["reason"] == "stale_result"
        assert service.call("open", file_id=original, event_id="stale-replacement")["reason"] == "stale_result"
        with sqlite3.connect(service.database) as connection:
            assert connection.execute("SELECT count(*) FROM opens WHERE event_id='replaced-open'").fetchone()[0] == 0
        with child.open("rb"), sqlite3.connect(service.database) as connection:
            connection.execute("BEGIN IMMEDIATE")
            child.unlink()
            folder.rmdir()
            folder.mkdir()
            child.write_bytes(b"replacement")
            connection.rollback()
            wait_for(lambda: service.paths().get(os.fsencode(child)) != child_id)
        assert service.indexed(folder) != folder_id
        assert service.call("resolve", file_id=child_id)["reason"] == "stale_result"
        # Replacements made while stopped must also be detected by startup repair.
        current = service.indexed(file)
        service.stop(crash=True)
        with file.open("rb"):
            file.unlink()
            file.write_bytes(b"restart replacement")
            service.start()
            wait_for(lambda: service.paths().get(os.fsencode(file)) != current)
        assert service.call("resolve", file_id=current)["reason"] == "stale_result"
    finally:
        service.stop()


def large_ids_and_retention(service):
    path = service.root / "precise.txt"
    path.write_bytes(b"")
    service.command("index", "--db", str(service.database), "--config", str(service.config))
    large = 9007199254740993
    with sqlite3.connect(service.database) as connection:
        connection.execute("UPDATE files SET id=? WHERE path=?", (large, os.fsencode(path)))
        connection.execute("INSERT INTO searches VALUES('expired','old',0)")
        connection.execute("INSERT INTO opens VALUES('expired-open',?,'expired',0)", (large,))
    service.start()
    try:
        assert service.indexed(path) == str(large)
        assert service.command("resolve", "--null", str(large)) == os.fsencode(path) + b"\0"
        def expired():
            with sqlite3.connect(service.database) as connection:
                return connection.execute("SELECT count(*) FROM searches WHERE id='expired'").fetchone()[0] == 0 and connection.execute("SELECT count(*) FROM opens WHERE event_id='expired-open'").fetchone()[0] == 0
        wait_for(expired)
        assert service.call("open", file_id=str(large), event_id="without-search", search_id="unretained")["status"] == "ok"
        def missing_search_is_null():
            with sqlite3.connect(service.database) as connection:
                return connection.execute("SELECT file_id,search_id FROM opens WHERE event_id='without-search'").fetchall() == [(large, None)]
        wait_for(missing_search_is_null)
    finally:
        service.stop()


def slow_output_and_response_limit(service):
    directory = service.root
    for i in range(3):
        directory = directory / (str(i) + "d" * 150)
        directory.mkdir()
    for i in range(1000):
        (directory / (f"file-{i:04d}-" + "x" * 150 + ".txt")).touch()
    service.start()
    try:
        def ready():
            return not service.call("status")["indexing"]["active"] and service.call("status")["indexing"]["reconciliations"] > 0
        wait_for(ready)
        assert service.call("query", query="/", limit=1000)["reason"] == "response_limit"
        assert service.call("query", query="file-0001-", limit=1)["status"] == "ok"
        with service.connect() as stalled:
            stalled.sendall(b'{"version":1,"request_id":"slow-output","op":"query","query":"/","limit":500}\n')
            for _ in range(10):
                assert service.call("status")["status"] == "ok"
            time.sleep(5.2)
            output = bytearray()
            while True:
                chunk = stalled.recv(65536)
                if not chunk:
                    break
                output.extend(chunk)
            # The response exceeds the Unix socket send buffer, so the stalled
            # client is disconnected before its entire frame can be transmitted.
            assert output and not output.endswith(b"\n")
    finally:
        service.stop()


with tempfile.TemporaryDirectory(prefix="torchlight-daemon-") as temporary:
    primary = Path(temporary) / "primary"
    primary.mkdir(mode=0o700)
    live_catalog(Service(primary))
    replacements = Path(temporary) / "replacements"
    replacements.mkdir(mode=0o700)
    rapid_replacements(Service(replacements))
    fallback = Path(temporary) / "fallback"
    fallback.mkdir(mode=0o700)
    watch_exhaustion_and_disabled_history(Service(fallback, "--watch-capacity", "1", "--no-history"))
    precision = Path(temporary) / "precision"
    precision.mkdir(mode=0o700)
    large_ids_and_retention(Service(precision, "--history-days", "1"))
    slow = Path(temporary) / "slow"
    slow.mkdir(mode=0o700)
    slow_output_and_response_limit(Service(slow))
print("Daemon integration tests passed.")
