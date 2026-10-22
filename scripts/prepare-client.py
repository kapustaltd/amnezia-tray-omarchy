#!/usr/bin/env python3
"""Apply the bridge to an exact, clean upstream checkout; never touch /opt."""
import argparse
from pathlib import Path
import shutil
import subprocess

PIN = "de93650a90739b87bb47a632872ea9d0adc9412f"
ROOT = Path(__file__).resolve().parents[1]


def run(source, *args):
    return subprocess.run(["git", "-C", str(source), *args], check=True, text=True, capture_output=True).stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Clean AmneziaVPN 5.0.3.0 checkout")
    args = parser.parse_args()
    source = args.source.resolve()
    if run(source, "rev-parse", "HEAD") != PIN:
        parser.error(f"Expected upstream 5.0.3.0 at {PIN}")
    if run(source, "status", "--porcelain", "--untracked-files=normal"):
        parser.error("Upstream checkout must be clean; keep build output outside it")
    destination = source / "client/omarchy"
    if destination.exists():
        parser.error("client/omarchy already exists")
    patch = ROOT / "patches/amnezia-5.0.3.0-bridge.patch"
    run(source, "apply", "--check", str(patch))
    run(source, "apply", str(patch))
    destination.mkdir()
    for name in ("traybridge.h", "traybridge.cpp"):
        shutil.copy2(ROOT / "bridge" / name, destination / name)
    print(f"Bridge prepared in {source}")
    print("Configure upstream with -DAMNEZIA_OMARCHY_BRIDGE=ON. No installed files were changed.")


if __name__ == "__main__":
    main()
