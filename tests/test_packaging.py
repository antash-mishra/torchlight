"""Repository publishing checks for packaging/deb/publish_apt.sh: build a stand-in .deb,
publish it into a site folder given by a relative path (as the release workflow does) and
signed by a throwaway key, then check as an ordinary user that apt accepts the repository
and finds the package, and rejects it when it trusts another key. Also runs the package's
maintainer scripts with stand-in systemctl and deb-systemd-helper commands, checking what
they do on first install, upgrade, removal and purge, and builds a package with
packaging/deb/build_deb.sh from stand-in programs to check where its daemon unit and
autostart entry go. Skips when dpkg-deb, apt-ftparchive or gpg is missing, and the build
check when cc or dpkg-shlibdeps is.

usage: test_packaging.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

repo = Path(__file__).resolve().parent.parent
missing = [tool for tool in ["dpkg-deb", "apt-ftparchive", "apt-get", "gpg"] if shutil.which(tool) is None]
if missing:
    print("Packaging check skipped: missing " + ", ".join(missing))
    raise SystemExit(0)
VERSION = "9.9.9"


def run(command, **options):
    return subprocess.run(command, check=True, capture_output=True, text=True, **options)


def throwaway_key(home):
    """A signing key in its own GnuPG home; returns its fingerprint."""
    home.mkdir(mode=0o700)
    env = dict(os.environ, GNUPGHOME=str(home))
    run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "", "--quick-gen-key",
         "Torchlight test signing", "ed25519", "sign", "never"], env=env)
    listing = run(["gpg", "--list-keys", "--with-colons"], env=env).stdout
    return next(line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr"))


def stand_in_package(base):
    root = base / "package"
    (root / "DEBIAN").mkdir(parents=True)
    (root / "DEBIAN/control").write_text(
        f"Package: torchlight\nVersion: {VERSION}\nArchitecture: amd64\n"
        "Maintainer: Test <test@example.invalid>\nDescription: stand-in for the publishing check\n")
    deb = base / f"torchlight_{VERSION}_amd64.deb"
    run(["dpkg-deb", "--root-owner-group", "--build", str(root), str(deb)])
    return deb


def apt_update(base, site, keyring):
    """apt-get update against the site as an ordinary user; returns (ok, apt-cache policy)."""
    state = base / ("apt-" + keyring.stem)
    for folder in ["etc/apt/sources.list.d", "etc/apt/preferences.d", "etc/apt/apt.conf.d",
                   "var/lib/apt/lists/partial", "var/cache/apt/archives/partial", "var/lib/dpkg"]:
        (state / folder).mkdir(parents=True, exist_ok=True)
    (state / "var/lib/dpkg/status").touch()
    (state / "etc/apt/sources.list.d/torchlight.sources").write_text(
        f"Types: deb\nURIs: file://{site}/apt\nSuites: stable\nComponents: main\n"
        f"Architectures: amd64\nSigned-By: {keyring}\n")
    options = ["-o", f"Dir::Etc={state}/etc/apt", "-o", "Dir::Etc::sourcelist=/dev/null",
               "-o", f"Dir::State={state}/var/lib/apt", "-o", f"Dir::State::status={state}/var/lib/dpkg/status",
               "-o", f"Dir::Cache={state}/var/cache/apt", "-o", "Debug::NoLocking=1",
               "-o", f"APT::Sandbox::User={os.environ.get('USER') or os.getlogin()}"]
    update = subprocess.run(["apt-get", *options, "update", "--error-on=any"], capture_output=True, text=True)
    policy = subprocess.run(["apt-cache", *options, "policy", "torchlight"], capture_output=True, text=True)
    return update.returncode == 0, update.stdout + update.stderr, policy.stdout


def maintainer_calls(base, script, *arguments):
    """Run a maintainer script with stand-ins that log their calls; one user (1000) is
    logged in. Returns the logged calls."""
    stubs = base / "stubs"
    stubs.mkdir(exist_ok=True)
    log = base / "calls.log"
    log.write_text("")
    for name, reply in [("systemctl", 'case "$*" in *list-units*) echo "user@1000.service loaded active running User Manager for UID 1000";; esac'),
                        ("deb-systemd-helper", "")]:
        stub = stubs / name
        stub.write_text(f'#!/bin/sh\necho "{name} $*" >> "{log}"\n{reply}\nexit 0\n')
        stub.chmod(0o755)
    env = dict(os.environ, PATH=f"{stubs}:{os.environ['PATH']}")
    run(["sh", str(repo / "packaging/deb" / script), *arguments], env=env)
    return log.read_text().splitlines()


def check_maintainer_scripts(base):
    user = "systemctl --quiet --user --machine 1000@"
    first = maintainer_calls(base, "postinst", "configure")
    assert "deb-systemd-helper --user enable torchlightd.service" in first, first
    assert f"{user} daemon-reload" in first and f"{user} start torchlightd.service" in first, first
    upgrade = maintainer_calls(base, "postinst", "configure", "0.0.9")
    assert f"{user} try-restart torchlightd.service" in upgrade, upgrade
    assert f"{user} start torchlightd.service" not in upgrade, upgrade
    assert f"{user} stop torchlightd.service" in maintainer_calls(base, "prerm", "remove")
    assert maintainer_calls(base, "prerm", "upgrade", "0.1.0") == []
    assert maintainer_calls(base, "postrm", "remove") == ["deb-systemd-helper --user mask torchlightd.service"]
    assert maintainer_calls(base, "postrm", "purge") == [
        "deb-systemd-helper --user purge torchlightd.service",
        "deb-systemd-helper --user unmask torchlightd.service"]


def check_deb_layout(base):
    """The built package's unit runs /usr/bin/torchlightd for graphical-session.target, and
    its autostart entry, a conffile, starts that unit at login on desktops that never start
    the target, which sessions systemd runs skip (ADR 0059)."""
    if shutil.which("cc") is None or shutil.which("dpkg-shlibdeps") is None:
        print("Package layout check skipped: missing cc or dpkg-shlibdeps")
        return
    programs = base / "programs"
    programs.mkdir()
    (programs / "main.c").write_text("int main(void) { return 0; }\n")
    for program in ["torchlight", "torchlightd", "torchlight-gtk"]:
        run(["cc", str(programs / "main.c"), "-o", str(programs / program)])
    deb = Path(run([str(repo / "packaging/deb/build_deb.sh"), str(base / "deb"),
                    str(programs)]).stdout.strip())
    control = base / "control"
    run(["dpkg-deb", "-e", str(deb), str(control)])
    assert (control / "conffiles").read_text().split() == [
        "/etc/apt/sources.list.d/torchlight.sources", "/etc/xdg/autostart/torchlightd.desktop"]
    tree = base / "tree"
    with subprocess.Popen(["dpkg-deb", "--fsys-tarfile", str(deb)], stdout=subprocess.PIPE) as dpkg:
        with tarfile.open(fileobj=dpkg.stdout, mode="r|") as tar:
            tar.extractall(tree, filter="tar")
    unit = (tree / "usr/lib/systemd/user/torchlightd.service").read_text().splitlines()
    assert "ExecStart=/usr/bin/torchlightd" in unit and "WantedBy=graphical-session.target" in unit
    entry = (tree / "etc/xdg/autostart/torchlightd.desktop").read_text().splitlines()
    for line in ["Exec=systemctl --user start torchlightd.service", "TryExec=/usr/bin/torchlightd",
                 "X-systemd-skip=true"]:
        assert line in entry, (line, entry)


with tempfile.TemporaryDirectory(prefix="torchlight-packaging-") as temporary:
    base = Path(temporary)
    fingerprint = throwaway_key(base / "gnupg")
    deb = stand_in_package(base)
    # A relative site path, as the workflow passes it: publishing runs from inside apt/.
    env = dict(os.environ, GNUPGHOME=str(base / "gnupg"), GPG_KEY=fingerprint)
    run([str(repo / "packaging/deb/publish_apt.sh"), "site", str(deb)], cwd=base, env=env)
    site = base / "site"
    for name in ["index.html", ".nojekyll", "apt/torchlight.sources", "apt/torchlight-archive-keyring.gpg",
                 f"apt/pool/main/t/torchlight/{deb.name}", "apt/dists/stable/InRelease",
                 "apt/dists/stable/Release.gpg", "apt/dists/stable/main/binary-amd64/Packages.gz"]:
        assert (site / name).is_file(), name
    trusted = base / "trusted.gpg"
    exported = subprocess.run(["gpg", "--export", fingerprint], env=env, check=True, capture_output=True)
    trusted.write_bytes(exported.stdout)
    ok, log, policy = apt_update(base, site, trusted)
    assert ok, log
    assert f"Candidate: {VERSION}" in policy, policy
    # The release key did not sign this repository, so apt must refuse it.
    ok, log, policy = apt_update(base, site, repo / "packaging/deb/torchlight-archive-keyring.gpg")
    assert not ok and "NO_PUBKEY" in log, log
    check_maintainer_scripts(base)
    check_deb_layout(base)
print("Packaging check passed: published through a relative path, signed, accepted by apt, "
      "refused with another key; maintainer scripts enable, start, restart and stop the daemon; "
      "the package starts it at login")
