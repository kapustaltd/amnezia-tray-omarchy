# Verification — 2026-10-22

Local environment: Omarchy 4.0.4, x86_64, GCC 16.2.1, Qt 6.11.2.

Completed:

- Built the tray executable with qmake6 and make.
- Built and passed the Qt/D-Bus suite: **10 passed, 0 failed** (including setup
  and teardown). It checks malformed/oversized requests, snapshot privacy,
  real-state propagation, protocol IDs versus row numbers, active-connection
  guards, import confirmation, allowed settings, cancelable resume, exported
  D-Bus surface, tray actions and second-instance `vpn://` forwarding.
- Rendered and visually checked the native tray menu in an offscreen Qt session.
- Applied the preparation script to a clean AmneziaVPN 5.0.3.0 checkout at
  `de93650a90739b87bb47a632872ea9d0adc9412f`.
- Built the complete patched AmneziaVPN GUI **and VPN service** using upstream
  CMake/Conan dependencies and `AMNEZIA_OMARCHY_BRIDGE=ON`.
- Started the rebuilt GUI on a private D-Bus, with empty XDG configuration/data
  directories and a private temporary socket directory. Its real API returned an
  empty profile list, accepted native page navigation, rejected invalid imports,
  and previewed a synthetic WireGuard configuration encoded in the original
  single-frame Amnezia QR format. No profile was committed and no tunnel was connected.
- Checked a generated QR image using the same `zbarimg` invocation as the wrapper.
- Validated the desktop entry, shell syntax, Python syntax and patch whitespace.

The GUI smoke check is reproducible after building the backend:

```bash
python3 scripts/smoke-native.py \
  --client /path/to/rebuilt/AmneziaVPN --qdbus qdbus6
```

The check starts no privileged VPN service and never connects a real tunnel.

Not exercised against a real VPN/server: tunnel establishment, DNS changes,
kill-switch packet blocking, split-routing behavior, actual suspend/wake,
subscription gateway refresh, SSH provisioning, or export round trips against a
live server. These remain integration acceptance checks requiring a test profile
and test host. The user's installed client, profiles and service were not replaced.

The local build uses temporary dependency directories. Published source and
PKGBUILD are the reproducible deliverables; the wrapper package does not include
the patched backend or replace the upstream deployment process.
