"""Shared native-frame observations and X11 input for isolated popup checks."""
import base64
import json
import os
from pathlib import Path
import subprocess
import time


def wait_for(predicate, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.01)
    raise AssertionError("condition timed out")


class PopupDriver:
    def __init__(self, base, environment, xdotool):
        self.frames = base / "frames.jsonl"
        self.env = dict(environment,
                        TORCHLIGHT_TEST_POPUP_FRAMES=str(self.frames))
        probe = str(Path("build/test_popup_probe.so").resolve())
        assert Path(probe).exists(), "Build test_popup_probe.so before GUI checks"
        self.env["LD_PRELOAD"] = " ".join(filter(None, [probe, os.environ.get("LD_PRELOAD")]))
        self.xdotool = xdotool
        self.offset = 0
        self.observed = []
        self.input_ms = []

    def read_frames(self):
        if not self.frames.exists():
            return self.observed
        with self.frames.open() as stream:
            stream.seek(self.offset)
            while True:
                line = stream.readline()
                if not line or not line.endswith("\n"):
                    break
                item = json.loads(line)
                for key in ["query", "status"]:
                    item[key] = base64.b64decode(item.pop(key + "_b64")).decode()
                self.observed.append(item)
                self.offset = stream.tell()
        return self.observed

    def frame(self, predicate, timeout=12, after=0):
        def matching():
            for item in self.read_frames():
                if item["paint_us"] >= after and predicate(item):
                    return item
            return None
        return wait_for(matching, timeout)

    def xdo(self, *arguments, check=True):
        result = subprocess.run([self.xdotool, *map(str, arguments)], env=self.env,
                                capture_output=True, text=True, check=check, timeout=8)
        return result.stdout.strip()

    def focused(self):
        try:
            return self.xdo("getwindowname", self.xdo("getactivewindow")) == "Torchlight"
        except subprocess.CalledProcessError:
            return False

    def type_query(self, text, measure=True):
        started = time.monotonic_ns() // 1000
        self.xdo("key", "ctrl+a")
        self.xdo("type", "--clearmodifiers", "--delay", "0", text)
        item = self.frame(lambda f: f["query"] == text and f["ready"], after=started)
        if measure:
            self.input_ms.append((item["paint_us"] - item["changed_us"]) / 1000)
        return item

    def clear(self):
        started = time.monotonic_ns() // 1000
        self.xdo("key", "ctrl+a", "BackSpace")
        return self.frame(lambda f: f["query"] == "" and f["ready"] and f["rows"] == 0,
                          after=started)

    def geometry(self):
        window = self.xdo("getactivewindow")
        return {key: int(value) for key, value in
                (line.split("=", 1) for line in self.xdo("getwindowgeometry", "--shell", window).splitlines())}
