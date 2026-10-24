"""Exercise the opt-in Omarchy CLI in temporary homes, never the real desktop.

The harness substitutes only the os-release file at the platform boundary. External
desktop commands are stand-ins; assertions check generated files and launched routes.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "scripts/omarchy.py"
HARNESS = """
import runpy, sys
from pathlib import Path
scope = runpy.run_path(sys.argv[1])
scope['main'].__globals__['OS_RELEASE'] = Path(sys.argv[2])
sys.argv = sys.argv[3:]
sys.exit(scope['main']())
"""
TOOL = """#!/usr/bin/python3
import json, os, sys
from pathlib import Path
root = Path(os.environ['TEST_ROOT'])
name = Path(sys.argv[0]).name
args = sys.argv[1:]
if name == 'omarchy' and args == ['version']:
    print((root / 'version').read_text().strip())
elif name == 'omarchy' and args == ['theme', 'color', '--all']:
    colors = json.loads((root / 'colors.json').read_text())
    for key, value in colors.items(): print(key + '\\t' + value)
elif name == 'hyprctl' and args == ['configerrors']:
    pass
else:
    with (root / 'launched.jsonl').open('a') as log:
        log.write(json.dumps([name, *args]) + '\\n')
"""


class OmarchyIntegration(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="torchlight-omarchy-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / "home with spaces"
        self.config = self.home / "preferences"
        self.data = self.home / "share files"
        self.system = self.root / "omarchy"
        self.release = self.root / "os-release"
        self.release.write_text('ID="omarchy"\nID_LIKE=arch\n')
        (self.root / "version").write_text("4.0.4-1\n")
        self.colors = {"background": "#ffffff", "foreground": "#000000",
                       "dark_background": "#f5f5f5", "darker_background": "#e8e8e8",
                       "selection": "#c0c0c0", "muted": "#808080",
                       "red": "#2a2a2a", "green": "#3a3a3a", "yellow": "#4a4a4a",
                       "cyan": "#3e3e3e", "magenta": "#2e2e2e", "blue": "#1a1a1a"}
        (self.root / "colors.json").write_text(json.dumps(self.colors))
        tools = self.root / "bin"
        tools.mkdir()
        for name in ("omarchy", "omarchy-shell", "systemctl", "hyprctl", "torchlight-gtk", "pkill"):
            tool = tools / name
            tool.write_text(TOOL)
            tool.chmod(0o755)
        self.env = {**os.environ, "HOME": str(self.home), "XDG_CONFIG_HOME": str(self.config),
                    "XDG_DATA_HOME": str(self.data), "OMARCHY_PATH": str(self.system),
                    "PATH": f"{tools}:/usr/bin:/bin", "TEST_ROOT": str(self.root)}
        self.hypr = self.config / "hypr/hyprland.lua"
        self.hypr.parent.mkdir(parents=True)
        self.hypr.write_text('-- Existing user configuration\nprint("unchanged")\n')
        self.gtk = self.config / "gtk-4.0/gtk.css"
        self.gtk.parent.mkdir(parents=True)
        self.gtk.write_text('/* Existing GTK styling */\nlabel { font-size: 12px; }\n')
        self.menu = self.system / "default/omarchy/omarchy-menu.jsonc"
        self.menu.parent.mkdir(parents=True)
        self.menu.write_text('''{
          // Menu comments and a trailing comma, including URL/comma text in actions.
          "apps": {"label":"Apps", "provider":"apps"},
          "setup": {"label":"Setup"},
          "setup.monitors": {"label":"Monitors", "action":"echo https://example.org/,}"},
          "setup.hidden": {"label":"Hidden", "when":"false"},
          "setup.hidden.child": {"label":"Should not appear", "action":"false"},
          "setup.theme": {"label":"Theme", "action":"false"},
        }''')
        override = self.config / "omarchy/extensions/omarchy-menu.jsonc"
        override.parent.mkdir(parents=True)
        override.write_text('{"setup.monitors":{"label":"My displays"}}')

    def cli(self, *args, installed=False):
        script = self.data / "torchlight/omarchy/integration.py" if installed else SCRIPT
        return subprocess.run([sys.executable, "-c", HARNESS, str(script), str(self.release),
                               str(script), *args], env=self.env, capture_output=True, text=True)

    def success(self, *args, **options):
        result = self.cli(*args, **options)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_other_distributions_and_old_omarchy_are_untouched(self):
        before = {p: p.read_bytes() for p in self.home.rglob("*") if p.is_file()}
        for identity, version in (("arch", "4.0.4"), ("ubuntu", "4.0.4"), ("omarchy", "3.9.0")):
            self.release.write_text(f"ID={identity}\nID_LIKE=arch\n")
            (self.root / "version").write_text(version)
            for action in ("install", "sync", "launch", "uninstall"):
                self.assertNotEqual(self.cli(action).returncode, 0)
            after = {p: p.read_bytes() for p in self.home.rglob("*") if p.is_file()}
            self.assertEqual(before, after)
        self.assertFalse((self.root / "launched.jsonl").exists())

    def test_install_sync_launch_and_removal(self):
        original_hypr, original_gtk = self.hypr.read_bytes(), self.gtk.read_bytes()
        self.success("install")
        lua = self.config / "hypr/torchlight.lua"
        self.assertIn("border_size = 0", lua.read_text())
        self.assertNotIn("hl.unbind", lua.read_text())
        self.success("install")
        self.assertEqual(self.hypr.read_text().count('require("hypr.torchlight")'), 1)
        self.assertEqual(self.gtk.read_text().count("@import"), 1)
        applications = self.data / "applications"
        monitors = applications / "torchlight-omarchy-setup.monitors.desktop"
        self.assertIn("Name=Omarchy Setup — My displays", monitors.read_text())
        self.assertIn("resolution", monitors.read_text())
        self.assertFalse(list(applications.glob("*setup.hidden*")))
        self.assertFalse((applications / "torchlight-omarchy-apps.desktop").exists())
        self.assertEqual(len(list(applications.glob("*.desktop"))), 9)
        palette = self.config / "torchlight/omarchy/palette.css"
        self.assertIn("tk_surface #ffffff", palette.read_text())
        self.assertIn("tk_sel_bg #c0c0c0", palette.read_text())
        self.assertNotIn("{{", palette.read_text())
        self.success("launch", "setup.monitors", installed=True)
        self.success("launch", "torchlight.audio", installed=True)
        launched = (self.root / "launched.jsonl").read_text()
        self.assertIn('["omarchy", "menu", "summon", "setup.monitors"]', launched)
        self.assertIn('["omarchy-shell", "shell", "summon", "omarchy.audio"]', launched)
        self.assertNotEqual(self.cli("launch", "setup.hidden.child").returncode, 0)
        self.success("install", "--bind-super-space")
        self.assertIn('hl.unbind("SUPER + SPACE")', lua.read_text())
        self.success("install")
        self.assertIn('hl.unbind("SUPER + SPACE")', lua.read_text())
        self.colors.update(background="#1e1e2e", foreground="#cdd6f4", selection="#45475a")
        (self.root / "colors.json").write_text(json.dumps(self.colors))
        self.menu.write_text('{"setup":{"label":"Setup"}}')
        unrelated = applications / "my-app.desktop"
        unrelated.write_text("unrelated application")
        self.success("sync", installed=True)
        self.assertIn("tk_surface #1e1e2e", palette.read_text())
        self.assertIn("tk_ink #cdd6f4", palette.read_text())
        # A user-defined entry survives removal of the stock entry it overrides.
        self.assertTrue(monitors.exists())
        (self.config / "omarchy/extensions/omarchy-menu.jsonc").write_text("{}")
        self.success("sync", installed=True)
        self.assertFalse(monitors.exists())
        self.assertNotEqual(self.cli("launch", "setup.monitors", installed=True).returncode, 0)
        self.success("uninstall", installed=True)
        self.assertEqual(self.hypr.read_bytes(), original_hypr)
        self.assertEqual(self.gtk.read_bytes(), original_gtk)
        self.assertEqual(unrelated.read_text(), "unrelated application")
        self.assertFalse(lua.exists())
        self.assertFalse(palette.exists())
        self.assertFalse(list(applications.glob("torchlight-omarchy-*.desktop")))

    def test_does_not_replace_user_owned_files(self):
        lua = self.config / "hypr/torchlight.lua"
        lua.write_text("-- My own configuration\n")
        self.assertNotEqual(self.cli("install").returncode, 0)
        self.assertEqual(lua.read_text(), "-- My own configuration\n")
        self.assertFalse((self.data / "torchlight/omarchy/integration.py").exists())


if __name__ == "__main__":
    unittest.main()
