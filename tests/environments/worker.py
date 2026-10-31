"""Build and execute desktop images inside a disposable Boat VM, never on the host."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

LANES = ("x11", "gnome", "kde", "cinnamon")
ROOT = Path(__file__).resolve().parents[2]
CACHE = Path("/home/user/torchlight-matrix-images")


def run(command, log, timeout=3600):
    """Run a bounded command, retaining its complete output even on failure."""
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout, cwd=ROOT)


def docker_ready():
    """Wait briefly for Docker after a snapshot fork has restarted the VM."""
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        result = subprocess.run(["docker", "info"], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=15)
        if result.returncode == 0:
            return
        time.sleep(2)
    raise RuntimeError("Docker did not become ready within 90 seconds")


def image_id(lane):
    """Return the local image identity, or None when the fork has no such image."""
    result = subprocess.run(["docker", "image", "inspect", "--format={{.Id}}",
                             f"torchlight-matrix-{lane}"], capture_output=True, text=True,
                            timeout=30)
    return result.stdout.strip() if result.returncode == 0 else None


def load_images(required, output, cache=CACHE):
    """Restore and verify exported images; Boat forks may omit Docker's image store."""
    identities = json.loads((cache / "images.json").read_text())
    if any(lane not in identities for lane in required):
        raise RuntimeError("The image archive does not contain every requested environment")
    if any(image_id(lane) != identities[lane] for lane in required):
        run(["docker", "load", "--input", str(cache / "images.tar")],
            output / "load-images.log", timeout=900)
    if any(image_id(lane) != identities[lane] for lane in required):
        raise RuntimeError("Restored Docker image identities differ from the build manifest")


def save_images(identities, output, cache=CACHE):
    """Keep a portable image archive in the home filesystem that Boat snapshots preserve."""
    cache.mkdir(parents=True, exist_ok=True)
    temporary = cache / "images.tar.tmp"
    run(["docker", "save", "--output", str(temporary),
         *[f"torchlight-matrix-{lane}" for lane in identities]], output / "save-images.log",
        timeout=900)
    temporary.replace(cache / "images.tar")
    manifest = cache / "images.json.tmp"
    manifest.write_text(json.dumps(identities, indent=2) + "\n")
    manifest.replace(cache / "images.json")


def run_suite(lane, output):
    """Run as an unprivileged container user with private display/session resources."""
    output.mkdir(parents=True, exist_ok=True)
    log = output / "container.log"
    log.touch()
    subprocess.run(["sudo", "-n", "chown", "10001:10001", str(output)], check=True)
    # ASan's address-space layout needs this on recent cloud kernels, as in release CI.
    subprocess.run(["sudo", "-n", "sysctl", "-w", "vm.mmap_rnd_bits=28"], check=True)
    image = "base" if lane == "checks" else lane
    command = ["docker", "run", "--pull=never", "--rm", "--init", "--network=none", "--cap-drop=ALL",
               "--security-opt=no-new-privileges", "--shm-size=512m", "--pids-limit=1024",
               "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=1g",
               "--name", f"torchlight-matrix-{lane}",
               "--mount", f"type=bind,src={output},dst=/artifacts",
               f"torchlight-matrix-{image}", lane]
    try:
        run(command, log, timeout=3900)
    finally:
        # Also remove a container whose client was interrupted or timed out.
        subprocess.run(["docker", "rm", "-f", f"torchlight-matrix-{lane}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        # Retained session directories are private (0700); make them readable by SCP's user.
        subprocess.run(["sudo", "-n", "chown", "-R", "--no-dereference",
                        f"{os.getuid()}:{os.getgid()}", str(output)], check=True, timeout=60)


def build_image(lane, output, jobs):
    """Export BuildKit cache into the snapshotted home filesystem for future runs."""
    cache = CACHE / "build-cache" / lane
    temporary = cache.with_name(lane + ".new")
    cache.parent.mkdir(parents=True, exist_ok=True)
    if temporary.exists():
        shutil.rmtree(temporary)
    restore = ["--cache-from", f"type=local,src={cache}"] if (cache / "index.json").exists() else []
    run(["docker", "build", "--progress=plain", "--build-arg", f"JOBS={jobs}", *restore,
         "--cache-to", f"type=local,dest={temporary},mode=max",
         "-t", f"torchlight-matrix-{lane}", "-f",
         f"tests/environments/Dockerfile.{lane}", "."], output / f"build-{lane}.log")
    if cache.exists():
        shutil.rmtree(cache)
    temporary.rename(cache)


def build(output, lanes, jobs):
    """Compile once, check once, then build only the requested desktop layers."""
    image_ids = {}
    for lane in ("base", *lanes):
        print(f"Building {lane}", flush=True)
        build_image(lane, output, jobs)
        image_ids[lane] = image_id(lane)
        if not image_ids[lane]:
            raise RuntimeError(f"Built image {lane} is missing")
    (output / "images.json").write_text(json.dumps(image_ids, indent=2) + "\n")
    save_images(image_ids, output)
    run_suite("checks", output / "checks")
    # The portable archive is authoritative. Avoid Boat restoring unused images
    # in the background while each worker imports that same archive.
    run(["docker", "image", "prune", "--all", "--force"], output / "prune-images.log", timeout=300)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("build", "run"))
    parser.add_argument("--lane", choices=LANES)
    parser.add_argument("--lanes", nargs="+", choices=LANES, default=list(LANES))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.operation == "run" and not args.lane:
        parser.error("run requires --lane")
    args.output.mkdir(parents=True, exist_ok=True)
    docker_ready()
    if args.operation == "build":
        build(args.output.resolve(), args.lanes, args.jobs)
    else:
        output = args.output.resolve() / args.lane
        output.mkdir(parents=True, exist_ok=True)
        load_images([args.lane], output)
        run_suite(args.lane, output)


if __name__ == "__main__":
    main()
