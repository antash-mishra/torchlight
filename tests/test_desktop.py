"""M3 daemon integration: XDG overrides, locale, typed results and launch history."""
import itertools
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
replacement_library = str(Path(sys.argv[2] if len(sys.argv) > 2 else "build/test_desktop_replace.so").resolve())
sequence = itertools.count()


def wait_for(predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.025)
    raise AssertionError("condition timed out")


with tempfile.TemporaryDirectory(prefix="torchlight-m3-") as directory:
    base = Path(directory)
    root = base / "files"
    root.mkdir()
    (root / "Display notes.txt").touch()
    # An application-name token must survive a full popup of filename prefixes.
    for index in range(24):
        (root / f"chromepolicy-{index:02}.txt").touch()
    (root / "browser-notes.txt").touch()
    user = base / "user" / "applications"
    system = base / "system" / "applications"
    user.mkdir(parents=True)
    system.mkdir(parents=True)
    config = base / "config"
    config.write_text(f"root = {root}\n")
    database = base / "catalog.db"
    path = base / "torchlight.sock"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, XDG_DATA_HOME=str(user.parent),
               XDG_DATA_DIRS=str(system.parent), XDG_CURRENT_DESKTOP="X-Cinnamon",
               LANGUAGE="fr", LC_ALL="C.UTF-8")

    def entry(folder, name, fields):
        file = folder / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text("[Desktop Entry]\nType=Application\nExec=/bin/true\n" + fields)
        return file

    display = entry(system, "display.desktop", "Name=Display\nKeywords=screen;resolution;\nCategories=Settings;\n")
    entry(system, "editor.desktop", "Name=InstalledEditor\nCategories=Utility;\n")
    entry(system, "google-chrome.desktop", "Name=Google Chrome\nGenericName=Web Browser\n")
    entry(system, "keyword.desktop", "Name=KeywordOnly\nKeywords=chrome;\n")
    for index in range(12):
        entry(system, f"duplicate-{index:02}.desktop", "Name=Duplicate App\n")
    entry(system, "sound.desktop", "Name=Sound\nCategories=Settings;\n")
    entry(system, "keyboard.desktop", "Name=Keyboard\nCategories=Settings;\n")
    entry(system, "masked.desktop", "Name=InvisibleSystem\n")
    entry(user, "masked.desktop", "Hidden=true\n")
    entry(user, "private.desktop", "Name=InvisiblePrivate\nNoDisplay=true\n")
    entry(user, "incompatible.desktop", "Name=InvisibleOther\nOnlyShowIn=KDE;\n")
    entry(user, "override.desktop", "Name=OverrideUser\n")
    entry(system, "override.desktop", "Name=OverrideSystem\n")
    entry(system, "nested/tool.desktop", "Name=NestedTool\n")
    entry(system, "translated.desktop", "Name=EnglishEditor\nName[fr]=EditeurFrancais\nGenericName[fr]=TexteFrancais\nKeywords[fr]=motfrancais;\n")
    race = entry(system, "race.desktop", "Name=AtomicOriginal\n")
    replacement = base / "replacement.desktop"
    replacement.write_text("[Desktop Entry]\nType=Application\nName=AtomicReplacement\nExec=/bin/false\n")
    # Preload ASan first when testing an instrumented executable; the injection
    # library intercepts GIO's loader as well as our direct keyfile reads.
    linked = subprocess.run(["ldd", binary], check=True, capture_output=True, text=True).stdout
    runtimes = [line.split()[2] for line in linked.splitlines() if line.lstrip().startswith("libasan.so")]
    env.update(LD_PRELOAD=":".join([*runtimes, replacement_library]),
               TORCHLIGHT_TEST_DESKTOP_PATH=str(race),
               TORCHLIGHT_TEST_DESKTOP_REPLACEMENT=str(replacement))
    log = tempfile.TemporaryFile()
    daemon = subprocess.Popen([binary, "--config", str(config), "--db", str(database),
                               "--rescan-ms", "500"], env=env, stdout=log, stderr=log)

    def call(op, **fields):
        request = dict(version=1, request_id=str(next(sequence)), op=op, **fields)
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(8)
            connection.connect(str(path))
            connection.sendall(json.dumps(request).encode() + b"\n")
            with connection.makefile("rb") as stream:
                response = json.loads(stream.readline(1024 * 1024))
        assert response["request_id"] == request["request_id"]
        return response

    def ready():
        assert daemon.poll() is None
        try:
            return call("status")["status"] == "ok"
        except OSError:
            return False

    def query(text):
        response = call("query", query=text, limit=10)
        assert response["status"] == "ok", response
        return response

    try:
        wait_for(ready)
        initial = next(r for r in query("AtomicOriginal")["results"]
                       if r.get("desktop_id") == "race.desktop")
        assert initial["name"] == "AtomicOriginal"
        assert not replacement.exists(), "The replacement must occur during the first parsed read"
        changed = wait_for(lambda: next((r for r in query("AtomicReplacement")["results"]
                                        if r.get("name") == "AtomicReplacement"), None))
        assert changed["id"] != initial["id"]
        assert call("resolve", file_id=initial["id"])["reason"] == "stale_result"
        for text, expected in [("display", "display.desktop"), ("screen", "display.desktop"),
                               ("resolution", "display.desktop"), ("sound", "sound.desktop"),
                               ("keyboard", "keyboard.desktop"), ("NestedTool", "nested-tool.desktop"),
                               ("EditeurFrancais", "translated.desktop"), ("TexteFrancais", "translated.desktop"),
                               ("motfrancais", "translated.desktop")]:
            response = query(text)
            assert any(r.get("desktop_id") == expected for r in response["results"]), (text, response)
        assert any(r.get("kind") == "application" and r.get("name") == "InstalledEditor"
                   for r in query("InstalledEditor")["results"])
        assert not query("Invisible")["results"]
        assert any(r.get("name") == "OverrideUser" for r in query("OverrideUser")["results"])
        assert not any(r.get("name") == "OverrideSystem" for r in query("OverrideSystem")["results"])
        wait_for(lambda: any(r.get("kind") == "file" for r in query("Display notes")["results"]))
        for text in ["c", "ch", "chr", "chro", "chrom", "chrome", "CHROME", "google chr"]:
            for limit in [1, 10, 1000]:
                response = call("query", query=text, limit=limit)
                assert response["status"] == "ok", response
                assert response["results"][0].get("desktop_id") == "google-chrome.desktop", (text, limit, response)
        # Metadata-only matches keep their ordinary strength; this is not an
        # unconditional preference for every installed application.
        assert query("browser")["results"][0]["kind"] == "file"
        full = call("query", query="chrome", limit=1000)["results"]
        keyword = next(i for i, result in enumerate(full)
                       if result.get("desktop_id") == "keyword.desktop")
        assert all(i < keyword for i, result in enumerate(full) if result["kind"] == "file")
        exact_file = root / "chrome"
        exact_file.touch()
        wait_for(lambda: query("chrome")["results"][0].get("path") == str(exact_file))
        assert query(str(exact_file))["results"][0]["path"] == str(exact_file)
        exact_file.unlink()
        wait_for(lambda: query("chrome")["results"][0].get("desktop_id") == "google-chrome.desktop")
        tied = call("query", query="duplicate app", limit=1000)["results"]
        assert len(tied) == 12
        for limit in [1, 3, 10]:
            small = call("query", query="duplicate app", limit=limit)["results"]
            assert [r["id"] for r in small] == [r["id"] for r in tied[:limit]]
        assert query("")["results"][0]["kind"] == "folder"
        response = query("display")
        application = next(r for r in response["results"] if r.get("desktop_id") == "display.desktop")
        assert application["kind"] == "settings" and application["icon"]
        assert call("resolve", file_id=application["id"])["results"][0]["desktop_id"] == "display.desktop"
        for _ in range(2):
            assert call("open", file_id=application["id"], event_id="same-launch", search_id=response["search_id"])["status"] == "ok"
        def history():
            with sqlite3.connect(database) as db:
                return db.execute("SELECT count(*) FROM desktop_opens WHERE event_id='same-launch'").fetchone()[0] == 1
        wait_for(history)
        old = application["id"]
        display.write_text(display.read_text().replace("Exec=/bin/true", "Exec=/bin/false"))
        wait_for(lambda: any(r.get("desktop_id") == "display.desktop" and r["id"] != old for r in query("display")["results"]))
        assert call("resolve", file_id=old)["reason"] == "stale_result"
        display.unlink()
        wait_for(lambda: not any(r.get("desktop_id") == "display.desktop" for r in query("display")["results"]))
        entry(user, "added.desktop", "Name=NewlyInstalled\n")
        wait_for(lambda: any(r.get("desktop_id") == "added.desktop" for r in query("NewlyInstalled")["results"]))
        assert call("history_clear")["status"] == "ok"
        def cleared():
            with sqlite3.connect(database) as db:
                return db.execute("SELECT count(*) FROM desktop_opens").fetchone()[0] == 0
        wait_for(cleared)
    finally:
        daemon.terminate()
        daemon.wait(timeout=15)
        log.seek(0)
        output = log.read().decode(errors="replace")
        assert daemon.returncode == 0 and "AddressSanitizer" not in output and "runtime error:" not in output, output
        log.close()
print("Desktop integration passed: atomic replacement, overrides, localization, settings, updates and history.")
