# transmission-client-gtk

GTK UI for Transmission that can **control a remote daemon**—not only run its own embedded session.

## What makes this fork special

Upstream **transmission-gtk** (Transmission 4.x) still starts an in-process `tr_session` and cannot use `remote-session-*` settings. If you point stock GTK at RPC keys in `settings.json`, nothing happens; the app keeps its own empty or separate torrent list under `~/.config/transmission/`.

**transmission-client-gtk** adds what Qt already has:

- **HTTP JSON-RPC client** (`RpcClient`, libsoup) instead of only `tr_rpc_request_exec(session_, …)` on a local session.
- **Remote session lifecycle** in `Session.cc`: connect, poll `torrent-get` / `session-get` / `session-stats`, apply `session-set`, handle gzipped responses.
- **Remote-aware UI**: main window, torrent list, context menu, properties (peers, trackers, files), stats, filters, blocklist, add-torrent flows—all branch on `is_remote()` where needed.
- **Explicit session modes** in Preferences → Remote so you choose remote client vs local host vs local-only without guessing config keys.
- **Separate identity**: binary `transmission-client-gtk`, config `~/.config/transmission-client-gtk/`, so it does not fight your daemon’s or stock GTK’s settings.

Typical use case: **`transmission-daemon` on the server** (systemd, VPN, Sonarr) + **this app on the desktop** to see and control the same torrents with the GTK interface you already like.

### Compared to other options

| Client | GTK | Uses your existing daemon |
|--------|-----|---------------------------|
| transmission-gtk (upstream) | Yes | No |
| transmission-qt | No | Yes |
| transmission-remote-gtk | Yes | Yes (different codebase/UI) |
| **transmission-client-gtk** | Yes | Yes (this fork) |

Implementation is modeled on `qt/RpcClient.cc`, `qt/Session.cc`, and related Qt code in this tree (same GPL codebase).

## Config

Settings: `~/.config/transmission-client-gtk/settings.json`

**Preferences → Remote → Session mode**

| Mode | Behavior |
|------|----------|
| Act as remote | Connect to a daemon (`remote_session_enabled`) |
| Act as host | Local embedded session + RPC enabled |
| None | Local only, RPC disabled |

Changing mode requires an **application restart**.

Remote keys (underscore form, same as Qt):

```json
{
    "remote_session_enabled": true,
    "remote_session_host": "127.0.0.1",
    "remote_session_port": 9091,
    "remote_session_requires_authentication": true,
    "remote_session_username": "transmission",
    "remote_session_password": "your-rpc-password",
    "remote_session_url_base_path": "/transmission/",
    "remote_session_https": false
}
```

## Build

From the repository root (Arch: Transmission build deps + `libsoup3`):

```bash
cmake -B build -DENABLE_GTK=ON -DENABLE_QT=OFF -DENABLE_DAEMON=OFF -DENABLE_CLI=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target transmission-client-gtk
```

Install or run from `build/gtk/transmission-client-gtk`.

## Feature status

- [x] Rebrand binary and config dir
- [x] HTTP RPC client (libsoup)
- [x] Remote session startup
- [x] Torrent list via `torrent-get` polling
- [x] Gzip RPC responses (Gio::ZlibDecompressor; Qt uses QNetwork auto-decompress)
- [x] Remote `torrent-add` (magnet, `.torrent` file, URL)
- [x] Remote torrent actions (start/stop/verify/remove; list refresh after mutations)
- [x] `session-stats` polling for status bar
- [x] Copy magnet link, alt-speed toggle via `session-set`
- [x] Remote context menu (queue, verify, reannounce, open folder, set location, …)
- [x] Properties dialog in remote mode (info, options, peers, trackers, files)
- [x] Stats dialog via session-stats RPC
- [x] Session-get/set sync, tracker filter, blocklist, options on add
- [x] Preferences → Remote: session mode dropdown with context-specific panels
