"""Repository publishing checks for packaging/deb/publish_apt.sh: build a stand-in .deb,
publish it into a site folder given by a relative path (as the release workflow does) and
signed by a throwaway key, then check as an ordinary user that apt accepts the repository
and finds the package, and rejects it when it trusts another key. Skips when dpkg-deb,
apt-ftparchive or gpg is missing.

usage: test_packaging.py
"""
import os
from pathlib import Path
import shutil
import subprocess
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
print("Packaging check passed: published through a relative path, signed, accepted by apt, "
      "refused with another key")
