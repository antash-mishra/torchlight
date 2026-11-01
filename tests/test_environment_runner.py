"""Regressions for required desktop jobs, diagnostics, source transfer and cloud cleanup."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch
from desktop_support import session_directory, unavailable

ROOT = Path(__file__).resolve().parent.parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tests/environments" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


suite = load("suite")
matrix = load("boat_matrix")
worker = load("worker")


class EnvironmentTests(unittest.TestCase):
    def test_private_system_bus_works_and_stops_with_its_scope(self):
        missing = [tool for tool in ("dbus-daemon", "gdbus") if not shutil.which(tool)]
        if missing:
            reason = f"Private system bus check needs {', '.join(missing)}"
            if os.environ.get("TORCHLIGHT_TEST_REQUIRED") == "1":
                self.fail(reason)
            self.skipTest(reason)
        with tempfile.TemporaryDirectory() as directory:
            with suite.system_bus(Path(directory)) as environment:
                command = ["gdbus", "call", "--system", "--dest", "org.freedesktop.DBus",
                           "--object-path", "/org/freedesktop/DBus",
                           "--method", "org.freedesktop.DBus.ListNames"]
                env = dict(os.environ, **environment)
                connected = subprocess.run(command, env=env, capture_output=True, timeout=5)
                self.assertEqual(connected.returncode, 0, connected.stderr)
            stopped = subprocess.run(command, env=env, capture_output=True, timeout=5)
            self.assertNotEqual(stopped.returncode, 0)

    def test_transient_status_failure_does_not_discard_live_job(self):
        with tempfile.TemporaryDirectory() as directory:
            boat = matrix.Boat("unused", Path(directory))
            states = [RuntimeError("API unavailable"), {"running": True},
                      {"running": False, "exitCode": 0, "stdout": "finished"}]
            with patch.object(boat, "call", side_effect=states), patch.object(matrix.time, "sleep"):
                self.assertTrue(boat.wait("owned", 42, "fixture"))
            self.assertEqual((Path(directory) / "fixture-remote.log").read_text(), "finished")

    def test_fork_restores_missing_images_from_portable_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "images.json").write_text(json.dumps({"x11": "sha256:built"}))
            with patch.object(worker, "image_id", side_effect=[None, "sha256:built"]), \
                    patch.object(worker, "run") as restored:
                worker.load_images(["x11"], root, cache=root)
            restored.assert_called_once_with(
                ["docker", "load", "--input", str(root / "images.tar")],
                root / "load-images.log", timeout=900)

    def test_restored_image_must_match_build_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "images.json").write_text(json.dumps({"x11": "sha256:built"}))
            with patch.object(worker, "image_id", return_value="sha256:wrong"), \
                    patch.object(worker, "run"):
                with self.assertRaisesRegex(RuntimeError, "identities differ"):
                    worker.load_images(["x11"], root, cache=root)

    def test_archive_cannot_silently_omit_required_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "images.json").write_text(json.dumps({"base": "sha256:built"}))
            with self.assertRaisesRegex(RuntimeError, "every requested"):
                worker.load_images(["gnome"], root, cache=root)

    def test_window_fixture_builds_matcher_before_linking(self):
        # Force the fixture alone through make: without its library prerequisite,
        # parallel clean builds can link before Cargo has produced libfrizbee.a.
        env = {key: value for key, value in os.environ.items()
               if key not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")}
        result = subprocess.run(["make", "--dry-run", "--always-make",
                                 "build/test_window_sources", "PKG_CONFIG=true",
                                 "WAYLAND_SCANNER=true", "CARGO=matrix-cargo", "CC=matrix-cc"],
                                cwd=ROOT, env=env, capture_output=True, text=True, check=True)
        self.assertIn("matrix-cargo build", result.stdout)
        self.assertLess(result.stdout.index("matrix-cargo build"),
                        result.stdout.index("matrix-cc"))

    def test_required_missing_desktop_fails(self):
        for required, expected in (("1", 1), ("0", 0)):
            result = subprocess.run([sys.executable, str(ROOT / "tests/test_windows_gnome_shell.py")],
                                    env=dict(os.environ, TORCHLIGHT_TEST_REQUIRED=required,
                                             TORCHLIGHT_GNOME_ROOT="/nonexistent-torchlight-test"),
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, expected, result.stderr)

    def test_required_missing_bindings_fails(self):
        for script in ("test_windows_desktops.py", "test_popup_wayland.py"):
            result = subprocess.run([sys.executable, "-S", str(ROOT / "tests" / script)],
                                    env=dict(os.environ, TORCHLIGHT_TEST_REQUIRED="1"),
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("Required desktop check failed", result.stdout)

    def test_optional_check_can_skip(self):
        with patch.dict(os.environ, TORCHLIGHT_TEST_REQUIRED="0"):
            unavailable("fixture is absent")

    def test_artifacts_survive_failure_without_following_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(os.environ, TORCHLIGHT_TEST_ARTIFACTS=directory):
                with self.assertRaisesRegex(RuntimeError, "fixture failed"):
                    with session_directory("fixture-") as session:
                        root = Path(session)
                        (root / "desktop.log").write_text("failure detail")
                        (root / "database.db").write_text("do not copy")
                        (root / "external.log").symlink_to("/etc/passwd")
                        raise RuntimeError("fixture failed")
            files = list(Path(directory).rglob("*.log"))
            self.assertEqual([file.name for file in files], ["desktop.log"])
            self.assertEqual(files[0].read_text(), "failure detail")
            self.assertFalse(list(Path(directory).rglob("*.db")))

    def test_zero_exit_does_not_hide_a_skipped_test(self):
        with tempfile.TemporaryDirectory() as directory:
            result = suite.run_check("skip", [sys.executable, "-c",
                                     "print('Desktop check skipped: absent')"], Path(directory), 10)
            self.assertEqual(result["status"], "failed")
            self.assertEqual(result["skipped"], 1)

    def test_timeout_terminates_descendants(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "survived"
            child = ("import time,pathlib,signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                     f"time.sleep(.8); pathlib.Path({str(marker)!r}).touch()")
            parent = f"import subprocess,sys,time; subprocess.Popen([sys.executable,'-c',{child!r}]); time.sleep(5)"
            result = suite.run_check("timeout", [sys.executable, "-c", parent], Path(directory), .3)
            self.assertEqual(result["reason"], "timeout")
            time.sleep(.8)
            self.assertFalse(marker.exists())

    def test_source_archive_includes_edits_and_untracked_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "repo"
            root.mkdir()
            output = Path(directory) / "output"
            output.mkdir()
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("ignored\n")
            (root / "tracked").write_text("before")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "commit", "-qm", "fixture"], cwd=root, check=True)
            (root / "tracked").write_text("after")
            (root / "new").write_text("untracked")
            (root / "ignored").write_text("exclude")
            archive, source = matrix.source_archive(root, output)
            with tarfile.open(archive) as bundle:
                self.assertEqual(bundle.extractfile("tracked").read(), b"after")
                self.assertEqual(bundle.extractfile("new").read(), b"untracked")
                self.assertNotIn("ignored", bundle.getnames())
                self.assertNotIn(".git", bundle.getnames())
            self.assertEqual(len(source["source_sha256"]), 64)

    def test_cleanup_only_deletes_owned_ids_and_reports_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            boat = matrix.Boat("unused", Path(directory))
            boat.owned = ["owned-a", "owned-b"]
            calls = []
            def call(arguments):
                calls.append(arguments)
                if arguments[1] == "owned-b":
                    raise RuntimeError("temporary failure")
            with patch.object(boat, "call", side_effect=call), patch.object(matrix.time, "sleep"):
                failed = boat.cleanup()
            self.assertEqual([item["id"] for item in failed], ["owned-b"])
            self.assertEqual({args[1] for args in calls}, {"owned-a", "owned-b"})
            self.assertEqual(json.loads((Path(directory) / "cleanup.json").read_text()), failed)

    def test_provision_failure_still_journals_allocated_id(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / "boat"
            fake.write_text("#!/bin/sh\nprintf '%s\\n' '{\"event\":\"created\",\"id\":\"owned-failed\"}'\nexit 1\n")
            fake.chmod(0o755)
            boat = matrix.Boat(str(fake), root)
            with self.assertRaises(RuntimeError):
                boat.create(["new"])
            self.assertEqual(boat.owned, ["owned-failed"])
            self.assertEqual(json.loads((root / "sandboxes.json").read_text()), boat.owned)

    def test_all_workers_start_before_waiting_and_failure_does_not_stop_collection(self):
        events = []
        class FakeBoat:
            def create(self, args):
                events.append("create")
                return f"worker-{len(events)}"

            def start(self, sandbox, command):
                events.append("start")
                return 1

        def collect(boat, sandbox, pid, lane, output):
            events.append("collect")
            return dict(lane=lane, status="failed" if lane == "x11" else "passed", cases=[])

        with patch.object(matrix, "collect", side_effect=collect):
            reports = matrix.execute(FakeBoat(), "base", ["x11", "gnome"], 2, Path("unused"))
        self.assertEqual(events, ["create", "create", "start", "start", "collect", "collect"])
        self.assertEqual([item["status"] for item in reports], ["failed", "passed"])

    def test_collection_survives_status_failure_and_rejects_incomplete_results(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            boat = matrix.Boat("unused", output)
            def transfer(source, target):
                target.mkdir()
                (target / "result.json").write_text(json.dumps(dict(
                    lane="x11", status="passed", cases=[dict(status="passed", skipped=0)])))
            with patch.object(boat, "wait", side_effect=RuntimeError("status unavailable")), \
                    patch.object(boat, "transfer", side_effect=transfer) as copied:
                report = matrix.collect(boat, "owned", 1, "x11", output)
            copied.assert_called_once()
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["reason"], "status unavailable")
            self.assertTrue((output / "x11/result.json").exists())

    def test_collection_requires_all_four_x11_groups(self):
        self.assertEqual(len(suite.commands("x11", Path("unused"))), 4)
        for count, expected in ((3, "failed"), (4, "passed"), (5, "failed")):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                boat = matrix.Boat("unused", output)
                def transfer(source, target):
                    target.mkdir()
                    cases = [dict(status="passed", skipped=0) for _ in range(count)]
                    (target / "result.json").write_text(json.dumps(dict(
                        lane="x11", status="passed", cases=cases)))
                with patch.object(boat, "wait", return_value=True), \
                        patch.object(boat, "transfer", side_effect=transfer):
                    report = matrix.collect(boat, "owned", 1, "x11", output)
                self.assertEqual(report["status"], expected)

    def test_remote_command_uses_explicit_shell_and_preserves_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            boat = matrix.Boat("unused", Path(directory))
            command = "python3 '/path with spaces/worker.py' run --lane x11"
            with patch.object(boat, "call", return_value={"pid": 42}) as called:
                self.assertEqual(boat.start("owned", command), 42)
            called.assert_called_once_with(
                ["exec", "owned", "--detach", "--", "bash", "-lc", command])


if __name__ == "__main__":
    unittest.main()
