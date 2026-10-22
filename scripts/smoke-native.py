#!/usr/bin/env python3
"""Smoke-test a rebuilt GUI on a private D-Bus with empty XDG directories.

Does not start the VPN service, connect a tunnel, or commit a server profile.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--qdbus", default="qdbus6")
    parser.add_argument("--inside-private-bus", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    client = args.client.resolve()
    qdbus = shutil.which(args.qdbus)
    if not qdbus:
        parser.error("qdbus6 is required (Arch: qt6-tools)")
    if not args.inside_private_bus:
        return subprocess.call(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
            "--client", str(client), "--qdbus", qdbus, "--inside-private-bus"])
    with tempfile.TemporaryDirectory(prefix="amnezia-tray-native-smoke-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        for variable, leaf in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "runtime"), ("TMPDIR", "tmp")):
            target = root / leaf
            target.mkdir(mode=0o700)
            env[variable] = str(target)
        env.update(QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="", XDG_CURRENT_DESKTOP="",
            QSG_RHI_BACKEND="software", QT_QUICK_BACKEND="software", AMNEZIA_TRAY_BRIDGE="1")
        for variable in ("GNOME_KEYRING_CONTROL", "SSH_AUTH_SOCK", "WAYLAND_DISPLAY", "DISPLAY"):
            env.pop(variable, None)

        def request(value):
            reply = subprocess.run([qdbus, "org.kapustaltd.AmneziaTray1", "/org/kapustaltd/AmneziaTray1",
                "org.kapustaltd.AmneziaTray1.Request", json.dumps(value)],
                env=env, capture_output=True, text=True, timeout=5, check=True)
            return json.loads(reply.stdout)

        with (root / "client.log").open("w+") as log:
            process = subprocess.Popen([str(client)], env=env, cwd=root, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 25
                while True:
                    try:
                        snapshot = request({"command": "snapshot"})
                        break
                    except (subprocess.CalledProcessError, json.JSONDecodeError):
                        if process.poll() is not None or time.monotonic() >= deadline:
                            log.seek(0)
                            raise RuntimeError("Native API did not start:\n" + log.read()[-6000:])
                        time.sleep(0.2)
                assert snapshot["ok"] and snapshot["api"] == 1
                assert snapshot["servers"] == [], "Smoke test must have an empty profile store"
                assert not request({"command": "connect"})["ok"]
                assert not request({"command": "import", "data": "invalid-test-configuration"})["ok"]
                for page in ("home", "settings", "dns", "selfhost", "share"):
                    assert request({"command": "page", "page": page})["ok"], page
                    time.sleep(0.1)
                # A synthetic, non-routable WireGuard config, wrapped in the
                # original one-frame Amnezia QR format (QDataStream, magic 1984).
                config = ("[Interface]\nPrivateKey = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n"
                    "Address = 10.77.0.2/32\n\n[Peer]\n"
                    "PublicKey = AQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n"
                    "AllowedIPs = 0.0.0.0/0\nEndpoint = 192.0.2.1:51820\n").encode()
                frame = struct.pack(">hBBI", 1984, 1, 0, len(config)) + config
                payload = base64.urlsafe_b64encode(frame).rstrip(b"=")
                imported = request({"command": "import", "qr": True,
                    "data": base64.b64encode(payload).decode()})
                assert imported["ok"], imported
                assert request({"command": "snapshot"})["servers"] == []
                assert process.poll() is None
                print("Native smoke passed: empty-store snapshot, page navigation, invalid-import rejection, framed QR preview, no committed profile.")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
