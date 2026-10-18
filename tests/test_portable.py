"""Portable archive checks (ADR 0056). Builds archives with packaging/portable/build_tarball.sh
from small stand-in programs, checks their contents and that the build refuses programs with
a run path or an unexpected library, then runs install.sh as a person would, into temporary
homes, with stand-in systemctl, pkill and pgrep commands that record what they are asked
(so the machine's own daemon is never touched): a first install, an update while the old
programs are open, a session without systemd, removal, missing libraries, root and an
awkward home folder. Skips when cc, readelf or ldd is missing.

usage: test_portable.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

repo = Path(__file__).resolve().parent.parent
missing = [tool for tool in ["cc", "readelf", "ldd", "strip"] if shutil.which(tool) is None]
if missing:
    print("Portable archive check skipped: missing " + ", ".join(missing))
    raise SystemExit(0)
PROGRAMS = ["torchlight", "torchlightd", "torchlight-gtk"]
EXTENSION = "torchlight@antash-mishra.github.io"
# A stand-in program: prints its version, and as the daemon records that it started.
STAND_IN = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("torchlight %s\n", VERSION);
        return 0;
    }
    const char *log = getenv("STAND_IN_LOG");
    FILE *file = log != NULL ? fopen(log, "a") : NULL;
    if (file != NULL) {
        fprintf(file, "started %s\n", VERSION);
        fclose(file);
    }
    return 0;
}
"""


def run(command, **options):
    return subprocess.run(command, capture_output=True, text=True, **options)


def stand_ins(folder, version, flags=()):
    """The three programs as stand-ins built with cc, in folder."""
    folder.mkdir(parents=True)
    source = folder / "stand_in.c"
    source.write_text(STAND_IN)
    for program in PROGRAMS:
        result = run(["cc", f'-DVERSION="{version}"', str(source), *flags, "-o",
                      str(folder / program)])
        assert result.returncode == 0, result.stderr
    return folder


def build(base, binaries):
    """An archive from binaries, in a folder of its own; returns the build's result and the
    archive's path."""
    out = Path(tempfile.mkdtemp(prefix="out-", dir=base))
    result = run([str(repo / "packaging/portable/build_tarball.sh"), str(out), str(binaries)])
    archives = sorted(out.glob("torchlight-*-linux-*.tar.gz"))
    return result, archives[0] if archives else None


def extract(archive, into):
    """The archive's single top folder, extracted into into."""
    into.mkdir(parents=True)
    with tarfile.open(archive) as tar:
        tar.extractall(into, filter="tar")
    (top,) = into.iterdir()
    return top


def commands(folder, log, systemd=True, uid=None):
    """Stand-ins for systemctl, pkill and pgrep that append their arguments to log;
    systemctl reports no user manager without systemd, and pkill finds no process. With uid,
    id -u answers it."""
    folder.mkdir(parents=True, exist_ok=True)
    manager = "exit 0" if systemd else 'case "$*" in *show-environment*) exit 1;; esac; exit 0'
    scripts = {"systemctl": manager, "pkill": "exit 1", "pgrep": "exit 1"}
    if uid is not None:
        scripts["id"] = f'[ "$1" = -u ] && echo {uid} && exit 0; exec /usr/bin/id "$@"'
    for name, body in scripts.items():
        script = folder / name
        script.write_text(f'#!/bin/sh\nprintf "{name} %s\\n" "$*" >> "{log}"\n{body}\n')
        script.chmod(0o755)
    return folder


def install(top, home, tools, *arguments):
    env = {"HOME": str(home), "PATH": f"{tools}:/usr/bin:/bin", "LANG": "C.UTF-8",
           "XDG_RUNTIME_DIR": str(home / "run"), "STAND_IN_LOG": str(home / "daemon.log")}
    return run([str(top / "install.sh"), *arguments], env=env)


def check_contents(archive, version):
    """Everything install.sh needs, owned by root, programs and installer executable."""
    name = archive.name.removesuffix(".tar.gz")
    assert name.startswith(f"torchlight-{version}-linux-"), name
    with tarfile.open(archive) as tar:
        members = {member.name.removeprefix(name + "/"): member for member in tar.getmembers()}
        readme = tar.extractfile(f"{name}/README").read().decode()
    expected = {"install.sh", "README", "share/systemd/user/torchlightd.service",
                "etc/xdg/autostart/torchlightd.desktop",
                "share/applications/org.torchlight.Launcher.desktop",
                "share/fonts/torchlight/SchibstedGrotesk-VF.ttf",
                "share/fonts/torchlight/bedstead.otf", "share/doc/torchlight/LICENSE",
                "share/doc/torchlight/copyright", "share/doc/torchlight/OFL.txt",
                "share/doc/torchlight/CHANGELOG.md"}
    expected |= {f"bin/{program}" for program in PROGRAMS}
    expected |= {f"share/{shell}/extensions/{EXTENSION}/{file}"
                 for shell in ("gnome-shell", "cinnamon") for file in ("metadata.json", "extension.js")}
    files = {path for path, member in members.items() if member.isfile()}
    assert files == expected, files ^ expected
    for path, member in members.items():
        assert member.uid == 0 and member.gid == 0, path
        executable = path in ("install.sh",) or path.startswith("bin/")
        assert member.mode & 0o777 == (0o755 if executable or member.isdir() else 0o644), path
    assert f"Torchlight {version} for Linux" in readme


def check_refusals(base):
    """A run path or a library outside the platform list stops the build."""
    with_path = stand_ins(base / "run-path", "1", ["-Wl,-rpath,/nowhere"])
    result, archive = build(base, with_path)
    assert result.returncode != 0 and "has a run path" in result.stderr and archive is None
    library = base / "lib"
    library.mkdir()
    (library / "extra.c").write_text("int extra(void) { return 1; }\n")
    assert run(["cc", "-shared", "-fPIC", str(library / "extra.c"), "-o",
                str(library / "libextra.so")]).returncode == 0
    linked = stand_ins(base / "extra", "1", [f"-L{library}", "-Wl,--no-as-needed", "-lextra"])
    result, archive = build(base, linked)
    assert result.returncode != 0 and "needs libextra.so" in result.stderr and archive is None


def lines(path):
    return path.read_text().splitlines() if path.exists() else []


def check_installed(top, home, systemd):
    local = home / ".local"
    for program in PROGRAMS:
        installed = local / "bin" / program
        assert installed.read_bytes() == (top / "bin" / program).read_bytes(), program
        assert installed.stat().st_mode & 0o777 == 0o755, program
    entry = (local / "share/applications/org.torchlight.Launcher.desktop").read_text()
    assert f'Exec="{local}/bin/torchlight-gtk" --toggle' in entry.splitlines(), entry
    assert sorted(p.name for p in (local / "share/fonts/torchlight").iterdir()) == [
        "SchibstedGrotesk-VF.ttf", "bedstead.otf"]
    for shell in ("gnome-shell", "cinnamon"):
        assert (local / f"share/{shell}/extensions/{EXTENSION}/extension.js").is_file(), shell
    unit = local / "share/systemd/user/torchlightd.service"
    autostart = lines(home / ".config/autostart/torchlightd.desktop")
    assert unit.exists() == systemd
    if systemd:
        assert "ExecStart=%h/.local/bin/torchlightd" in lines(unit)
        # Desktops that never start graphical-session.target start the unit at login
        # (ADR 0059); sessions systemd runs skip the entry.
        for line in ["Exec=systemctl --user start torchlightd.service",
                     f"TryExec={local}/bin/torchlightd", "X-systemd-skip=true"]:
            assert line in autostart, (line, autostart)
    else:
        assert f'Exec="{local}/bin/torchlightd"' in autostart, autostart


def check_first_install_and_update(base, top, version):
    """With systemd: files in place, the daemon enabled and restarted. An update renames new
    programs over the old ones, so a program still open keeps its old file."""
    home = base / "home-systemd"
    (home / "run").mkdir(parents=True)
    log = home / "commands.log"
    tools = commands(base / "tools-systemd", log)
    result = install(top, home, tools)
    assert result.returncode == 0, result.stderr
    check_installed(top, home, systemd=True)
    assert lines(log) == ["systemctl --user show-environment", "systemctl --user daemon-reload",
                          "systemctl --user --quiet enable torchlightd.service",
                          "systemctl --user restart torchlightd.service"], lines(log)
    assert f"Installed torchlight {version} in {home}/.local/bin." in result.stdout
    assert f'"{home}/.local/bin/torchlight-gtk" --toggle' in result.stdout
    assert f"add {home}/.local/bin to your PATH" in result.stdout
    # A unit of the person's own, which systemd prefers, is pointed out.
    own_unit = home / ".config/systemd/user/torchlightd.service"
    own_unit.parent.mkdir(parents=True)
    own_unit.write_text("[Service]\nExecStart=/elsewhere/torchlightd\n")
    newer = stand_ins(base / "newer", "9.9.10")
    _, archive = build(base, newer)
    newer_top = extract(archive, base / "newer-archive")
    daemon = home / ".local/bin/torchlightd"
    with daemon.open("rb") as running:
        old_inode = os.fstat(running.fileno()).st_ino
        log.write_text("")
        result = install(newer_top, home, tools)
        assert result.returncode == 0, result.stderr
        assert daemon.stat().st_ino != old_inode
        # The open file is still the old program, untouched.
        assert running.read() == (top / "bin/torchlightd").read_bytes()
    check_installed(newer_top, home, systemd=True)
    assert "systemctl --user restart torchlightd.service" in lines(log)
    # The systemd unit's daemon is restarted, never killed.
    assert not any(line.startswith("pkill") for line in lines(log)), lines(log)
    assert "Installed torchlight 9.9.10" in result.stdout
    assert f"Your own {own_unit} comes before the installed unit" in result.stdout
    own_unit.unlink()
    return home, tools, log


def check_without_systemd(base, top, systemd_home):
    """No user manager: an autostart entry and a daemon started now. Moving an install made
    with systemd over drops its unit, and moving back replaces the entry, so only one way of
    starting the daemon remains."""
    home = base / "home-plain"
    (home / "run").mkdir(parents=True)
    log = home / "commands.log"
    tools = commands(base / "tools-plain", log, systemd=False)
    result = install(top, home, tools)
    assert result.returncode == 0, result.stderr
    check_installed(top, home, systemd=False)
    assert f"pkill -u {os.getuid()} -x torchlightd" in lines(log), lines(log)
    for _ in range(50):  # the daemon was started in the background
        if lines(home / "daemon.log"):
            break
        subprocess.run(["sleep", "0.1"], check=True)
    assert lines(home / "daemon.log") == ["started 9.9.9"]
    result = install(top, systemd_home, tools)
    assert result.returncode == 0, result.stderr
    check_installed(top, systemd_home, systemd=False)
    # Back to systemd: the entry that ran the daemon gives way to the unit's, and the
    # daemon it started is stopped, so two never compete for the index.
    log.write_text("")
    result = install(top, home, commands(base / "tools-back", log))
    assert result.returncode == 0, result.stderr
    check_installed(top, home, systemd=True)
    assert f"pkill -u {os.getuid()} -x torchlightd" in lines(log), lines(log)
    assert "systemctl --user restart torchlightd.service" in lines(log), lines(log)


def check_uninstall(base, top):
    """Removal disables the daemon and deletes every installed file, but keeps the index,
    history and settings."""
    home = base / "home-remove"
    (home / "run").mkdir(parents=True)
    log = home / "commands.log"
    tools = commands(base / "tools-remove", log)
    assert install(top, home, tools).returncode == 0
    data = home / ".local/share/torchlight/catalog.db"
    settings = home / ".config/torchlight/config"
    for kept in (data, settings):
        kept.parent.mkdir(parents=True)
        kept.write_text("kept\n")
    log.write_text("")
    result = install(top, home, tools, "--uninstall")
    assert result.returncode == 0, result.stderr
    assert "systemctl --user --quiet disable --now torchlightd.service" in lines(log), lines(log)
    assert lines(log)[-1] == "systemctl --user daemon-reload"
    # fontconfig's own cache of the fonts folder may remain under ~/.cache.
    remaining = sorted(str(path.relative_to(home)) for path in home.rglob("*")
                       if path.is_file() and not path.relative_to(home).parts[0] == ".cache")
    assert remaining == [".config/torchlight/config", ".local/share/torchlight/catalog.db",
                         "commands.log"], remaining
    assert f"{home}/.local/share/torchlight" in result.stdout


def check_missing_library(base, top):
    """A library the system lacks is named, with how to install GTK, and nothing is installed.
    The archive's popup is swapped for one that needs a library, which is then deleted."""
    top = Path(shutil.copytree(top, base / "needs-gone-archive"))
    library = base / "gone"
    library.mkdir()
    (library / "gone.c").write_text("int gone(void) { return 1; }\n")
    assert run(["cc", "-shared", "-fPIC", str(library / "gone.c"), "-o",
                str(library / "libtorchlight-gone.so")]).returncode == 0
    (library / "main.c").write_text("int main(void) { return 0; }\n")
    result = run(["cc", str(library / "main.c"), f"-L{library}", "-Wl,--no-as-needed",
                  "-ltorchlight-gone", "-o", str(top / "bin/torchlight-gtk")])
    assert result.returncode == 0, result.stderr
    shutil.rmtree(library)
    home = base / "home-missing"
    (home / "run").mkdir(parents=True)
    result = install(top, home, commands(base / "tools-missing", home / "commands.log"))
    assert result.returncode == 1
    assert "libtorchlight-gone.so => not found" in result.stderr, result.stderr
    assert "sudo dnf install gtk4" in result.stderr
    assert not (home / ".local").exists()


def check_refused_runs(base, top):
    """Root would install into root's home; a home folder whose path cannot be written into
    a desktop entry is refused before anything is copied."""
    home = base / "home-root"
    (home / "run").mkdir(parents=True)
    result = install(top, home, commands(base / "tools-root", home / "commands.log", uid=0))
    assert result.returncode == 1 and "without sudo" in result.stderr
    awkward = base / 'home "quoted"'
    (awkward / "run").mkdir(parents=True)
    result = install(top, awkward, commands(base / "tools-awkward", awkward / "commands.log"))
    assert result.returncode == 1 and "cannot handle" in result.stderr
    assert not (home / ".local").exists() and not (awkward / ".local").exists()


version_header = (repo / "include/torchlight/version.h").read_text()
VERSION = version_header.split('TORCHLIGHT_VERSION "', 1)[1].split('"', 1)[0]
with tempfile.TemporaryDirectory(prefix="torchlight-portable-") as temporary:
    base = Path(temporary)
    result, archive = build(base, stand_ins(base / "programs", "9.9.9"))
    assert result.returncode == 0, result.stderr
    check_contents(archive, VERSION)
    check_refusals(base)
    top = extract(archive, base / "archive")
    systemd_home, _, _ = check_first_install_and_update(base, top, "9.9.9")
    check_without_systemd(base, top, systemd_home)
    check_uninstall(base, top)
    check_missing_library(base, top)
    check_refused_runs(base, top)
print("Portable archive checks passed.")
