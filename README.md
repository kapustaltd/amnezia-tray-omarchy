# amnezia-tray-omarchy

A native Qt 6 tray wrapper for **AmneziaVPN on Omarchy / Arch Linux**. English
interface, original profiles and VPN engine, no separate credential database.

The wrapper requires the included **local API patch** in AmneziaVPN 5.0.3.0.
An unmodified client cannot provide these controls over its existing local socket.
This project does not replace an installed client automatically.

| Feature | Integration |
| --- | --- |
| Connect / disconnect | Original connection controller; actual state shown in tray |
| Servers | Select existing AmneziaVPN profiles |
| Protocols | Discover installed VPN containers and platform support; subscription protocol selection opens the native home screen |
| Status | Connection state, selected server/protocol, server endpoint and reported session traffic |
| Import | Paste `vpn://` or configuration text, open a config file, decode a QR image with `zbar` |
| URL handler | Single-instance `vpn://` dispatch; native import preview asks for confirmation |
| Share / QR / export | Original sharing window, including native formats and QR display |
| Self-hosted | Original SSH setup wizard for new or existing servers |
| Settings | Native DNS, kill switch and IP/application split tunneling screens |
| Startup / recovery | Tray autostart, native auto-connect/start-minimized settings, cancelable reconnect after suspend |

Closing the tray leaves the VPN running. Configuration imports are previewed in
AmneziaVPN before they are committed. Unsupported protocols stay unavailable;
the wrapper does not implement VPN protocols itself.

## Build the tray and run checks

On Arch, install `base-devel`, `qt6-base`, `qt6-declarative`, `qt6-svg` and `dbus`.
`zbar` is optional for QR image import.

```bash
bash scripts/build.sh
./build/tray/amnezia-tray-omarchy --help
```

The script builds the tray and bridge tests, then runs tests on a separate D-Bus
session. It does not connect a VPN or change your desktop configuration.

`packaging/PKGBUILD` builds the tray as an Arch package:

```bash
cd packaging
makepkg -s
```

The package includes the wrapper, desktop URL-handler entry and optional systemd
user service. **It does not contain a rebuilt AmneziaVPN backend.** Install that
backend separately as described below.

## Prepare the original client

Use a separate source checkout. The script rejects a different commit or existing changes.

```bash
git clone --filter=blob:none https://github.com/amnezia-vpn/amnezia-client.git ../amnezia-native
git -C ../amnezia-native checkout de93650a90739b87bb47a632872ea9d0adc9412f
python3 scripts/prepare-client.py ../amnezia-native
git -C ../amnezia-native submodule update --init --recursive
```

Install the [upstream build prerequisites](https://github.com/amnezia-vpn/amnezia-client/tree/5.0.3.0#building)
(Qt 6.10+, CMake, Conan 2 and a C++ toolchain), then enable the bridge:

```bash
conan profile detect  # only if a suitable profile does not exist yet
cmake -S ../amnezia-native -B ../amnezia-native-build \
  -DCMAKE_BUILD_TYPE=Release -DAMNEZIA_OMARCHY_BRIDGE=ON
cmake --build ../amnezia-native-build --parallel 4
```

Follow upstream deployment instructions for its executable, libraries, resources
and VPN service. Reuse the original **AmneziaVPN** identity and profile storage.
Deploy compatible client and service versions together. The preparation script
does not install or copy files into `/opt/AmneziaVPN`.

The patched GUI publishes a session D-Bus API and hides its duplicate tray icon.
`AMNEZIA_TRAY_BRIDGE=0` disables the API and restores the original tray behavior.
Close the original client before starting a rebuilt copy: its single-instance
check otherwise raises the old process and exits before initialization.

## Run

Point the wrapper at the deployed, bridge-enabled executable:

```bash
./build/tray/amnezia-tray-omarchy --client /path/to/AmneziaVPN --start-client
```

The executable path is saved; you can change it in **Settings**. **Start
AmneziaVPN** launches it; **Open AmneziaVPN** raises its home window. The wrapper
runs without root privileges.

After installing the package, register links if your desktop has not selected
this handler:

```bash
xdg-mime default amnezia-tray-omarchy.desktop x-scheme-handler/vpn
```

Use **Start tray at login** in Settings, or the optional user service:

```bash
systemctl --user enable --now amnezia-tray-omarchy.service
```

Choose one autostart method. The separate native startup setting controls
AmneziaVPN itself. URL links import with confirmation; they do not silently
connect an untrusted configuration.

## Compatibility and verification

- Backend source is pinned to AmneziaVPN **5.0.3.0**. Other versions require
  rechecking the patch and controller contracts.
- Sharing, DNS, routing and self-hosted provisioning use original windows.
  Availability follows protocol, profile access and platform.
- The endpoint is the saved server hostname/IP, not a measured VPN exit IP.
- Traffic comes from original protocol counters. Missing counters show
  **Traffic: not reported by this protocol**.
- QR image import supports a single QR payload; multipart image assembly and
  live camera scanning are not implemented in the wrapper.
- Tunnel establishment, DNS/kill-switch behavior and SSH deployment need a test
  profile and server. Automated tests use controller fixtures and do not connect
  a real VPN or provision a remote server.

See [SPEC.md](SPEC.md) for decisions and acceptance criteria, and
[docs/LOCAL_API.md](docs/LOCAL_API.md) for the integration contract.
The [verification report](docs/VERIFICATION.md) records the local full-backend
build, native API smoke check, test results and remaining live-server checks.

GPL-3.0-only. Independent project; not an official AmneziaVPN release. The upstream
patch preserves the original files' license notices.
