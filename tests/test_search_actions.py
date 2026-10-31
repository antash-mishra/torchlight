"""M9 daemon/CLI contracts against a deterministic external Flatpak adapter."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

CLI, DAEMON = (str(Path(arg).resolve()) for arg in (sys.argv[1:3] if __name__ == "__main__" else ("build/torchlight", "build/torchlightd")))


def wait_for(check):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.03)
    raise AssertionError("Condition did not become true")


class Service:
    def __init__(self, base, settings=""):
        self.base = Path(base)
        self.root = self.base / "root"
        self.root.mkdir(exist_ok=True)
        self.path = self.base / "socket"
        self.config = self.base / "config"
        self.config.write_text(f"root = {self.root}\n{settings}")
        self.log = tempfile.TemporaryFile()
        self.env = dict(os.environ, HOME=str(self.base),
                        XDG_CONFIG_HOME=str(self.base / "config-home"),
                        XDG_DATA_HOME=str(self.base / "data"),
                        XDG_DATA_DIRS=str(self.base / "empty"),
                        XDG_RUNTIME_DIR=str(self.base),
                        FLATPAK_USER_DIR=str(self.base / "flatpak-user"),
                        FLATPAK_SYSTEM_DIR=str(self.base / "flatpak-system"),
                        FLATPAK_CONFIG_DIR=str(self.base / "flatpak-config"),
                        M9_FIXTURE=str(self.base),
                        PATH=f"{self.base / 'bin'}:{os.environ['PATH']}")
        self.process = None
        self.counter = 0

    def start(self):
        self.process = subprocess.Popen(
            [DAEMON, "--db", str(self.base / "catalog.db"), "--config", str(self.config),
             "--socket", str(self.path)], env=self.env, stdout=self.log, stderr=self.log)
        wait_for(self.ready)

    def ready(self):
        if self.process.poll() is not None:
            self.log.seek(0)
            raise AssertionError(self.log.read().decode(errors="replace"))
        try:
            return self.call("status")
        except (OSError, ValueError):
            return False

    def call(self, op, version=1, **fields):
        self.counter += 1
        request = dict(version=version, op=op, request_id=str(self.counter), **fields)
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(5)
            client.connect(str(self.path))
            client.sendall(json.dumps(request).encode() + b"\n")
            with client.makefile("rb") as stream:
                response = json.loads(stream.readline())
                assert response["phase"] == "final", response
                assert response["version"] == version, response
                return response

    def cli(self, command, *args, success=True):
        result = subprocess.run([CLI, command, "--socket", str(self.path), *args],
                                env=self.env, capture_output=True, timeout=10)
        assert (result.returncode == 0) == success, result.stderr
        assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
        return result

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=10)
        self.log.seek(0)
        log = self.log.read()
        assert self.process.returncode == 0, log
        assert b"AddressSanitizer" not in log and b"runtime error:" not in log, log
        self.log.close()


def fake_flatpak(base):
    """The real adapter must parse output and source configuration, without shell interpolation."""
    (base / "bin").mkdir()
    script = base / "bin/flatpak"
    script.write_text("""#!/usr/bin/python3
import os, sys, time
from pathlib import Path
b = Path(os.environ["M9_FIXTURE"])
with (b / "calls").open("a") as f: f.write(repr(sys.argv[1:]) + "\\n")
a = sys.argv[1:]
if "--system" in a: sys.exit(0)
if (b / "fail").exists(): sys.exit(1)
if "remotes" in a: print("flathub\\t\\ntesting\\t")
elif "list" in a:
    kind = "app/" if (b / "full-installed-ref").exists() else ""
    print("flathub\\t" + kind + "org.example.Editor/x86_64/stable")
elif "remote-ls" in a:
    remote = a[-1]
    if (b / "stall").exists(): time.sleep(20)
    print("app/org.example.Editor/x86_64/stable\\tExample Editor\\tEdit source code")
    if remote == "flathub":
        print("app/org.example.Other/x86_64/stable\\tExample Editor\\tDifferent publisher")
elif "remote-info" in a: print(a[-1])
elif "update" in a: pass
else: sys.exit(2)
""")
    script.chmod(0o755)
    root = base / "flatpak-user/repo"
    root.mkdir(parents=True)
    (root / "config").write_text(
        '[remote "flathub"]\nurl=https://example.invalid/one\n'
        '[remote "testing"]\nurl=https://example.invalid/two\n')


def contracts(base):
    fake_flatpak(base)
    service = Service(base)
    for i in range(20):
        (service.root / f"needle-{i:02}.txt").write_text("")
    (service.root / "web literal").write_text("")
    service.start()
    try:
        status = wait_for(lambda: (r := service.call("status"))["software"]["state"] == "ready" and r)
        assert status["capabilities"]["search_actions"] == 2
        assert status["capabilities"]["software_suggestions"] == 1
        assert status["software"]["count"] == 3, status
        wait_for(lambda: len(service.call("query", query="needle", limit=10)["results"]) == 10)
        legacy = service.call("query", query="needle", limit=10)
        current = service.call("query", version=2, mode="local", web_fallback=True, query="needle", limit=10)
        assert legacy["results"] == current["results"]
        assert len(current["results"]) == 10 and len(current["actions"]) == 1
        assert "actions" not in legacy
        assert "software_suggestions" not in current
        def suggestions(query):
            return service.call("query", version=2, mode="local", web_fallback=True,
                                software_suggestions=True, query=query, limit=10)
        auto = suggestions("example editor")
        assert auto["prefer_software"] and len(auto["software_suggestions"]) == 1, auto
        assert auto["software_suggestions"][0]["target"]["app_id"] == "org.example.Other"
        assert not suggestions("source code")["software_suggestions"]
        assert not suggestions("example.editor")["software_suggestions"]
        assert len(suggestions("needle")["results"]) == 10
        (service.root / "Example Editor").touch()
        wait_for(lambda: any(Path(r["path"]).name == "Example Editor"
                            for r in suggestions("Example Editor")["results"]))
        assert not suggestions("Example Editor")["prefer_software"]
        (service.root / "Example Editor").unlink()
        # A native desktop app suppresses install suggestions without pretending
        # it is the same package as any Flatpak source.
        apps = base / "data/applications"
        apps.mkdir(parents=True, exist_ok=True)
        native = apps / "native-editor.desktop"
        native.write_text("[Desktop Entry]\nType=Application\nName=Example Editor\nExec=/bin/true\n")
        wait_for(lambda: any(r.get("desktop_id") == "native-editor.desktop"
                            for r in suggestions("Example Editor")["results"]))
        assert not suggestions("Example Editor")["software_suggestions"]
        native.unlink()
        wait_for(lambda: suggestions("Example Editor")["software_suggestions"])
        assert current["actions"][0] == {"kind": "web", "provider": "duckduckgo", "query": "needle"}
        literal = service.cli("query", "--null", "web literal").stdout
        assert literal == os.fsencode(service.root / "web literal") + b"\0"
        assert service.call("query", version=2, mode="local", query="needle")["actions"] == []
        web = service.call("query", version=2, mode="web", query="café &+?#")
        assert not web["results"] and web["actions"][0]["query"] == "café &+?#"
        assert service.cli("web", "café &+?#").stdout == b"https://duckduckgo.com/?q=caf%C3%A9%20%26%2B%3F%23\n"
        service.cli("web", "--null", "cats", success=False)
        service.cli("software", "--null", "editor", success=False)
        software = service.call("query", version=2, mode="software", query="editor", limit=10)
        assert software["actions"] == [] and len(software["results"]) == 3
        refs = [row["target"] for row in software["results"]]
        assert len({(r["remote"], r["app_id"]) for r in refs}) == 3
        installed = [r for r in software["results"] if r["installed"]]
        assert len(installed) == 1 and installed[0]["target"]["remote"] == "flathub"
        for row in software["results"]:
            assert row["kind"] == "software" and "path" not in row and "id" not in row
        target = installed[0]["target"]
        assert service.call("resolve", version=2, target=target)["results"][0] == installed[0]
        stale = dict(target, revision=str(int(target["revision"]) + 1))
        assert service.call("resolve", version=2, target=stale)["reason"] == "stale_result"
        before = (base / "calls").read_text()
        for _ in range(30):
            service.call("query", version=2, mode="software", query="edit", limit=10)
            suggestions("example editor")
        assert (base / "calls").read_text() == before, "Typing spawned a package command"
        output = service.cli("software", "editor").stdout
        assert b"org.example.Editor" in output and b"user/flathub" in output
        assert output.count(b"\tinstalled\n") == 1 and output.count(b"\tavailable\n") == 2
        # Installed refs differ from remote-ls refs on real Flatpak 1.14.
        generation = service.call("status")["software"]["catalog_gen"]
        (base / "full-installed-ref").touch()
        service.cli("software-refresh")
        wait_for(lambda: service.call("status")["software"]["catalog_gen"] > generation)
        alternate = service.call("query", version=2, mode="software", query="editor")
        assert alternate["results"] == software["results"], alternate
        (base / "fail").touch()
        service.cli("software-refresh")
        wait_for(lambda: service.call("status")["software"]["state"] == "stale")
        assert len(service.call("query", version=2, mode="software", query="editor")["results"]) == 3
        (base / "fail").unlink()
        config = base / "flatpak-user/repo/config"
        config.write_text(config.read_text().replace("/one", "/replacement"))
        service.cli("software-refresh")
        wait_for(lambda: service.call("status")["software"]["state"] == "ready")
        assert service.call("resolve", version=2, target=target)["reason"] == "stale_result"
        (base / "stall").touch()
        service.cli("software-refresh")
        start = time.monotonic()
        assert service.call("query", version=2, mode="software", query="editor")["status"] == "ok"
        assert time.monotonic() - start < 1
    finally:
        service.stop()


def disabled(base):
    service = Service(base, "web_search = false\nweb_engine = google\nsoftware_search = false\n")
    service.start()
    try:
        assert service.call("status")["software"]["state"] == "disabled"
        web = service.call("query", version=2, mode="web", query="cats")
        assert web["actions"] == [] and web["reason"] == "web_disabled"
        software = service.call("query", version=2, mode="software", query="editor")
        assert software["results"] == [] and software["software"]["state"] == "disabled"
    finally:
        service.stop()


def large_software_page(base):
    path = base / "socket"
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(path))
        listener.listen()

        def serve():
            for operation in ("status", "query"):
                client, _ = listener.accept()
                with client, client.makefile("rb") as stream:
                    request = json.loads(stream.readline())
                    assert request["op"] == operation
                    response = dict(version=request["version"], request_id=request["request_id"],
                                    phase="final", status="ok", results=[])
                    if operation == "status":
                        response["capabilities"] = dict(search_actions=2)
                    else:
                        assert request["limit"] == 1000 and request["mode"] == "software"
                        for i in range(1000):
                            target = dict(kind="software", backend="flatpak", installation="user",
                                          remote="flathub", app_id=f"org.example.Editor{i}",
                                          arch="x86_64", branch="stable", revision="1", source_revision="2")
                            response["results"].append(dict(kind="software", target=target,
                                name=f"Editor {i}", summary="Edit text", installed=False))
                    client.sendall(json.dumps(response).encode() + b"\n")

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        result = subprocess.run([CLI, "software", "--socket", str(path), "--limit", "1000", "editor"],
                                capture_output=True, timeout=10)
        thread.join(timeout=5)
        assert result.returncode == 0, result.stderr
        assert len(result.stdout.splitlines()) == 1000


def old_daemon(base, malformed=False):
    path = base / "socket"
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(path))
        listener.listen()
        requests = []

        def serve():
            client, _ = listener.accept()
            with client, client.makefile("rb") as stream:
                request = json.loads(stream.readline())
                requests.append(request)
                response = dict(version=1, request_id=request["request_id"],
                                phase="final", status="ok", results=[])
                if malformed:
                    response["capabilities"] = []
                client.sendall(json.dumps(response).encode() + b"\n")

        thread = threading.Thread(target=serve)
        thread.start()
        result = subprocess.run([CLI, "web", "--socket", str(path), "cats"], capture_output=True, timeout=10)
        thread.join(timeout=5)
        assert result.returncode != 0
        assert len(requests) == 1 and requests[0]["op"] == "status"
        if not malformed:
            assert b"does not support" in result.stderr
        else:
            assert b"does not support" not in result.stderr


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="torchlight-m9-") as temp:
        root = Path(temp)
        for name, test in (("contracts", contracts), ("disabled", disabled), ("large", large_software_page),
                           ("old", old_daemon), ("malformed", lambda b: old_daemon(b, True))):
            base = root / name
            base.mkdir(mode=0o700)
            test(base)
    print("Web/software daemon and CLI contracts passed.")
