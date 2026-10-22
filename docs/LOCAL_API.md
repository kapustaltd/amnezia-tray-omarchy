# Local API v1

The bridge is built into the **unprivileged AmneziaVPN GUI**, never its root
service. It runs on the current user's session D-Bus. There is no TCP listener,
shell-command endpoint, remote API, or second profile database.

- Service and interface: `org.kapustaltd.AmneziaTray1`
- Object: `/org/kapustaltd/AmneziaTray1`
- Method: `Request(s json) → s json`
- Limit: 1 MiB per JSON request.
- Response: `{"ok":true,...}` or `{"ok":false,"error":"..."}`.

The bridge checks the caller's Unix UID. Other applications running as the same
user can control the VPN; session D-Bus is not a boundary against a compromised
desktop account. Requests and configuration contents are not logged by the bridge
or tray. Original AmneziaVPN logging behavior is unchanged.

| command | fields | behavior |
| --- | --- | --- |
| `snapshot` | none | API version, actual connection state, selected profile, available profiles/containers, selected native settings, observed session byte deltas |
| `connect` | none | Ask the native controller to connect the selected profile |
| `disconnect` | none | Ask the native controller to disconnect and cancel pending resume |
| `server` | `id`: string | Select an existing native profile while disconnected |
| `protocol` | `id`: integer | Select an installed, platform-supported VPN container ID while disconnected |
| `page` | `page`: string | Raise the original window and navigate to an allowed page |
| `import` | `data`: string; optional `qr`: boolean | Parse using the native importer and open its confirmation preview |
| `setting` | `key`: string; `value`: boolean | Update an allowed native setting or resume preference |

Allowed pages: `home`, `settings`, `dns`, `killSwitch`, `split`, `appSplit`,
`share`, `selfhost`, `importPreview`, `protocols`.

Allowed settings: `autoConnect`, `autoStartEnabled`, `startMinimized`,
`resumeAfterSleep`. DNS, kill switch and routing use the native settings pages so
the original validation, explanations and protocol restrictions still apply.

For `qr:true`, `data` is standard Base64 of the decoded QR payload bytes, not an
image. Text imports carry the original configuration string. The importer does
not commit a profile or connect automatically: the user confirms in AmneziaVPN.
Pending import previews cannot be silently replaced by another URL.

Snapshots whitelist model fields. They do not return private keys, passwords,
raw configuration, or administrative SSH credentials. `received` and `sent` are
decimal strings to preserve 64-bit precision. They accumulate native traffic
deltas since the current observed connection attempt. Protocols that do not
emit traffic counters report `hasTraffic:false`. The endpoint is the saved
server hostname/address, not an independently measured public exit IP.

A successful command acknowledges delivery to the native UI controller, not
successful establishment of the VPN tunnel. Poll `snapshot` for the outcome.
The shell polls asynchronously every two seconds and disables control when the
bridge is unavailable. Native error dialogs remain available in the original UI.

Subscription protocol choice delegates to the native home window because it can
require an API refresh and account-specific validation. Self-hosted/native VPN
container IDs are discovered from the original model; they are never row numbers
or a hard-coded protocol list.

The bridge listens to login1's `PrepareForSleep` signal on the system bus. It
attempts one resume connection five seconds after waking, only if VPN was active
before suspend and the preference is enabled. Explicit disconnects from either
the wrapper or native UI cancel that attempt. This does not replace AmneziaVPN's
own connection recovery logic.

The shell has a separate single-instance service:
`org.kapustaltd.AmneziaTrayShell1`, object
`/org/kapustaltd/AmneziaTrayShell1`, method `OpenUrl(s url) → b`.
Only `vpn://` links (maximum 512 KiB) or an empty string to raise the client are
accepted. Links are forwarded to the bridge in memory, never passed to a child
AmneziaVPN process as command-line arguments.
