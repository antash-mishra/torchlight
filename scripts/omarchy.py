#!/usr/bin/env python3
"""Opt-in Omarchy 4 integration: XDG settings, theme colors and Hyprland popup rules.

Run after installing Torchlight: python3 scripts/omarchy.py install [--bind-super-space].
Only this explicit command changes desktop configuration; ordinary installs are unchanged.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

OS_RELEASE = Path("/etc/os-release")
MARKER = "Managed by Torchlight's optional Omarchy integration"
REQUIRE = 'require("hypr.torchlight")'
PREFIX = "torchlight-omarchy-"
PALETTE = {
    "surface": "background", "panel": "dark_background", "ink": "foreground",
    "muted": "muted", "faint": "muted", "line": "darker_background",
    "sel_bg": "selection", "sel_ink": "foreground",
    **{color: color for color in ("red", "green", "yellow", "cyan", "magenta", "blue")},
}
PANELS = {
    "audio": ("Audio", "sound;volume;microphone", "audio"),
    "bluetooth": ("Bluetooth", "bluetooth", "bluetooth"),
    "display": ("Display", "screen;brightness", "monitor"),
    "wifi": ("Wi-Fi", "network;wireless;internet", "network"),
    "power": ("Power", "battery;power-profile", "power"),
}


def require_omarchy():
    """Reject other distributions before reading menus or writing user configuration."""
    release = dict(line.split("=", 1) for line in OS_RELEASE.read_text().splitlines()
                   if "=" in line and not line.startswith("#"))
    if shlex.split(release.get("ID", '""')) != ["omarchy"]:
        raise ValueError("This integration is only for Omarchy, not other Arch/Hyprland systems.")
    version = subprocess.check_output(["omarchy", "version"], text=True).strip()
    if not re.match(r"(?:[4-9]|[1-9][0-9]+)\.", version):
        raise ValueError("This integration requires Omarchy 4 or newer (Lua configuration).")


class Integration:
    """Own the per-user integration files; never edit packaged Omarchy files."""

    def __init__(self):
        self.home = Path.home()
        self.config = Path(os.environ.get("XDG_CONFIG_HOME", self.home / ".config"))
        self.data = Path(os.environ.get("XDG_DATA_HOME", self.home / ".local/share"))
        self.omarchy = Path(os.environ.get("OMARCHY_PATH", "/usr/share/omarchy"))
        self.script = self.data / "torchlight/omarchy/integration.py"
        self.palette = self.config / "torchlight/omarchy/palette.css"
        self.lua = self.config / "hypr/torchlight.lua"
        self.hyprland = self.config / "hypr/hyprland.lua"
        self.gtk = self.config / "gtk-4.0/gtk.css"
        self.hook = self.config / "omarchy/hooks/theme-set.d/torchlight"
        self.units = self.config / "systemd/user"
        self.import_line = f'@import url("{self.palette}");'

    def write(self, path, text):
        """Publish a complete generated file; leave unchanged files' timestamps alone."""
        if path.exists() and path.read_text() == text:
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        # A theme hook and the periodic sync may run at the same time.
        with tempfile.TemporaryDirectory(dir=path.parent, prefix=".torchlight-") as folder:
            temporary = Path(folder) / "new"
            temporary.write_text(text)
            temporary.replace(path)

    def menu(self):
        """Merge trusted Omarchy JSONC menu data and user overrides, plus bar panels."""
        items = {"root": {"label": "Menu"}}
        sources = [self.omarchy / "default/omarchy/omarchy-menu.jsonc",
                   self.config / "omarchy/extensions/omarchy-menu.jsonc"]
        for source in sources:
            if not source.exists():
                continue
            # Skip quoted strings while stripping comments and trailing commas: shell
            # actions may themselves contain URLs or comma/brace sequences.
            quoted = r'"(?:\\.|[^"\\])*"'
            raw = re.sub(quoted + r"|//[^\n]*|/\*.*?\*/",
                         lambda m: m[0] if m[0].startswith('"') else "",
                         source.read_text(), flags=re.S)
            raw = re.sub(quoted + r"|,\s*(?=[}\]])",
                         lambda m: m[0] if m[0].startswith('"') else "", raw)
            parsed = json.loads(raw)
            for key, value in parsed.get("items", parsed).items():
                items[key] = {**items.get(key, {}), **value}
        for key, (label, keywords, panel) in PANELS.items():
            items["torchlight." + key] = {
                "label": label, "parent": "setup", "aliases": keywords.split(";"),
                "panel": panel,
            }
        return items

    def lineage(self, items, key):
        """Return an entry's ancestors, rejecting cycles in user menu data."""
        chain = []
        while key:
            if key in chain:
                raise ValueError(f"Cycle in Omarchy menu: {key}")
            chain.append(key)
            key = items.get(key, {}).get("parent", key.rpartition(".")[0])
        return list(reversed(chain))

    def visible(self, items, key, guards):
        """Honor menu visibility, including parents; never run an entry's action here."""
        for ancestor in self.lineage(items, key):
            condition = items.get(ancestor, {}).get("when")
            if condition and condition not in guards:
                try:
                    guards[condition] = subprocess.run(
                        ["bash", "-lc", condition], timeout=5,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    ).returncode == 0
                except subprocess.TimeoutExpired:
                    guards[condition] = False
            if condition and not guards[condition]:
                return False
        return True

    def sync(self):
        """Refresh the palette and available settings, removing only our stale entries."""
        self.sync_palette()
        self.sync_settings()

    def sync_palette(self):
        """Resolve colors through Omarchy's own alias/fallback rules, never raw TOML."""
        colors = dict(line.split("\t", 1) for line in subprocess.check_output(
            ["omarchy", "theme", "color", "--all"], text=True).splitlines())
        template = self.palette_template()
        for key in PALETTE.values():
            value = colors[key]
            if not re.fullmatch(r"#[0-9a-fA-F]{6}", value):
                raise ValueError(f"Unsupported Omarchy palette color: {key}={value}")
            template = template.replace("{{ " + key + " }}", value)
        self.write(self.palette, template)

    def sync_settings(self):
        """Publish settings as XDG entries; ordinary apps already use the XDG catalog."""
        items, guards, expected = self.menu(), {}, set()
        directory = self.data / "applications"
        directory.mkdir(parents=True, exist_ok=True)
        for key, item in items.items():
            if key == "apps" or not re.fullmatch(r"[A-Za-z0-9_.-]+", key):
                continue
            if not self.visible(items, key, guards):
                continue
            labels = [items.get(part, {}).get("label", part.rsplit(".", 1)[-1])
                      for part in self.lineage(items, key)]
            name = "Omarchy " + " — ".join(labels)
            aliases = item.get("aliases", [])
            if isinstance(aliases, str):
                aliases = [aliases]
            keywords = ["Omarchy", "settings", *key.split("."), *aliases]
            if key == "setup.monitors":
                keywords += ["screen", "display", "resolution"]
            # Desktop values escape backslashes and line breaks, not shell syntax.
            name = name.replace("\\", "\\\\").replace("\n", r"\n").replace("\r", r"\r")
            text = (f"# {MARKER}\n[Desktop Entry]\nType=Application\nName={name}\n"
                    f'Exec=/usr/bin/python3 "{self.script}" launch {key}\n'
                    "Icon=preferences-system\nTerminal=false\nCategories=Settings;\n"
                    "Keywords=" + ";".join(keywords).replace("\n", " ") + ";\n")
            path = directory / f"{PREFIX}{key}.desktop"
            expected.add(path)
            if path.exists() and MARKER not in path.read_text():
                raise ValueError(f"Refusing to overwrite an existing file: {path}")
            self.write(path, text)
        for path in directory.glob(f"{PREFIX}*.desktop"):
            if path not in expected and MARKER in path.read_text():
                path.unlink()
        print(f"Synced {len(expected)} Omarchy settings entries.")

    def palette_template(self):
        """Define only Torchlight's namespaced colors, leaving other GTK apps alone."""
        definitions = [f"/* {MARKER} */"]
        definitions += [f"@define-color tk_{name} {{{{ {key} }}}};"
                        for name, key in PALETTE.items()]
        definitions += ["@define-color tk_edge alpha({{ foreground }}, 0.12);",
                        "@define-color tk_shadow_near alpha(black, 0.12);",
                        "@define-color tk_shadow_far alpha(black, 0.3);"]
        return "\n".join(definitions) + "\n"

    def install(self, bind):
        """Install opt-in desktop integration, preserving existing user config text."""
        for path in (self.config, self.data):
            if not path.is_absolute() or re.search(r'[\n\r"\\%$`]', str(path)):
                raise ValueError("Integration paths must be absolute, without shell/Exec metacharacters.")
        executable = shutil.which("torchlight-gtk")
        if executable is None:
            raise ValueError("Install Torchlight and put torchlight-gtk on PATH first.")
        bind = bind or (self.lua.exists() and 'hl.unbind("SUPER + SPACE")' in self.lua.read_text())
        owned = [self.script, self.palette, self.lua, self.hook,
                 self.units / "torchlight-omarchy.service",
                 self.units / "torchlight-omarchy.timer"]
        for path in owned:
            if path.exists() and MARKER not in path.read_text():
                raise ValueError(f"Refusing to overwrite an existing file: {path}")
        if not self.hyprland.exists():
            raise ValueError("Expected Omarchy 4's hypr/hyprland.lua configuration.")
        self.write(self.script, Path(__file__).read_text())
        self.sync()
        self.install_desktop(executable, bind)
        self.install_services()
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
        subprocess.run(["systemctl", "--user", "enable", "--now", "torchlight-omarchy.timer"], check=True)
        subprocess.run(["hyprctl", "reload"], check=True)
        errors = subprocess.check_output(["hyprctl", "configerrors"], text=True).strip()
        if errors:
            raise ValueError(f"Hyprland reported configuration errors: {errors}")
        print("Omarchy integration installed. Restart Torchlight to load its new colors.")

    def install_desktop(self, executable, bind):
        """Add isolated GTK and Lua config, leaving keybindings alone unless requested."""
        gtk = self.gtk.read_text() if self.gtk.exists() else ""
        if self.import_line not in gtk.splitlines():
            self.write(self.gtk, self.import_line + "\n" + gtk)
        lua = (f"-- {MARKER}\n"
               'o.window("org[.]torchlight[.]Launcher", { float = true, center = true, border_size = 0 })\n')
        if bind:
            lua += ('hl.unbind("SUPER + SPACE")\n'
                    f'o.bind("SUPER + SPACE", "Torchlight", {json.dumps(shlex.quote(executable) + " --toggle", ensure_ascii=False)})\n')
        self.write(self.lua, lua)
        config = self.hyprland.read_text()
        if REQUIRE not in config.splitlines():
            self.write(self.hyprland, config + "\n" + REQUIRE + "\n")

    def install_services(self):
        """Keep menu guards current and reload the palette after theme changes."""
        self.write(self.hook, f'#!/bin/sh\n# {MARKER}\nset -e\n'
                   f'/usr/bin/python3 "{self.script}" sync\n'
                   'pkill -u "$(id -u)" -x torchlight-gtk || [ "$?" -eq 1 ]\n')
        self.hook.chmod(0o755)
        self.write(self.units / "torchlight-omarchy.service",
                   f"# {MARKER}\n[Unit]\nDescription=Refresh Torchlight's Omarchy settings\n"
                   f'[Service]\nType=oneshot\nExecStart=/usr/bin/python3 "{self.script}" sync\n')
        self.write(self.units / "torchlight-omarchy.timer",
                   f"# {MARKER}\n[Unit]\nDescription=Keep Torchlight's Omarchy settings current\n"
                   "[Timer]\nOnStartupSec=30s\nOnUnitActiveSec=5min\n"
                   "[Install]\nWantedBy=timers.target\n")

    def launch(self, key):
        """Revalidate a result against the current menu before delegating its action."""
        items = self.menu()
        if key not in items or not self.visible(items, key, {}):
            raise ValueError("This Omarchy setting is no longer available.")
        panel = items[key].get("panel")
        command = ["omarchy-shell", "shell", "summon", "omarchy." + panel] if panel else [
            "omarchy", "menu", "summon", key]
        os.execvp(command[0], command)

    def uninstall(self):
        """Remove only integration-owned files and exact config lines we added."""
        subprocess.run(["systemctl", "--user", "disable", "--now", "torchlight-omarchy.timer"], check=True)
        subprocess.run(["systemctl", "--user", "stop", "torchlight-omarchy.service"], check=True)
        for path, line in ((self.gtk, self.import_line), (self.hyprland, REQUIRE)):
            if path.exists():
                text = path.read_text()
                self.write(path, text.replace("\n" + REQUIRE + "\n", "") if path == self.hyprland
                           else "".join(part for part in text.splitlines(keepends=True)
                                        if part.rstrip("\n") != line))
        paths = [self.script, self.palette, self.lua, self.hook,
                 self.units / "torchlight-omarchy.service",
                 self.units / "torchlight-omarchy.timer"]
        paths += list((self.data / "applications").glob(f"{PREFIX}*.desktop"))
        for path in paths:
            if path.exists() and MARKER in path.read_text():
                path.unlink()
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
        subprocess.run(["hyprctl", "reload"], check=True)
        print("Omarchy integration removed; Torchlight itself and your other settings are unchanged.")


def main():
    """CLI boundary: refuse unsupported systems and report integration failures."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "sync", "launch", "uninstall"))
    parser.add_argument("entry", nargs="?")
    parser.add_argument("--bind-super-space", action="store_true")
    args = parser.parse_args()
    try:
        require_omarchy()
        integration = Integration()
        if args.action == "install":
            integration.install(args.bind_super_space)
        elif args.action == "sync":
            integration.sync()
        elif args.action == "launch" and args.entry:
            integration.launch(args.entry)
        elif args.action == "uninstall":
            integration.uninstall()
        else:
            parser.error("launch requires an entry ID")
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"Omarchy integration: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
