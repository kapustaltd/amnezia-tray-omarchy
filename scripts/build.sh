#!/usr/bin/env bash
set -euo pipefail
tray_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$tray_root/build/tray" "$tray_root/build/tests"
cd "$tray_root/build/tray"
qmake6 "$tray_root/tray.pro"
make -j"${JOBS:-4}"
cd "$tray_root/build/tests"
qmake6 "$tray_root/tests/tests.pro"
make -j"${JOBS:-4}"
QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= XDG_CURRENT_DESKTOP= dbus-run-session -- ./bridge-tests
