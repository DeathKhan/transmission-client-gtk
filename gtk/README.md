# transmission-client-gtk

GTK remote client for an existing Transmission daemon (fork of upstream `transmission-gtk`).

Unlike stock Transmission GTK, this build **does not** embed a local daemon by default. It connects to a running `transmission-daemon` over HTTP RPC (same model as `transmission-qt`).

Implementation follows `qt/RpcClient.cc`, `qt/Session.cc`, and related Qt code in this tree (same GPL codebase).

## Config

Settings directory: `~/.config/transmission-client-gtk/settings.json`

**Preferences → Remote** has a **Session mode** dropdown:

| Mode | Behavior |
|------|----------|
| Act as remote | Connect to a daemon (`remote_session_enabled`) |
| Act as host | Local embedded session + RPC enabled |
| None | Local only, RPC disabled |

Changing mode requires an **application restart**.

Remote connection keys (underscore form, same as Qt):

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

From the Transmission source tree (Arch: install `transmission` build deps + `libsoup3`):

```bash
cmake -B build -DENABLE_GTK=ON -DENABLE_QT=OFF -DENABLE_DAEMON=OFF -DENABLE_CLI=OFF ...
cmake --build build --target transmission-client-gtk
```

## Status

- [x] Rebrand binary and config dir
- [x] HTTP RPC client (libsoup)
- [x] Remote session startup
- [x] Torrent list via `torrent-get` polling
- [x] Gzip RPC responses (Qt QNetwork decompresses automatically; we use Gio::ZlibDecompressor)
- [x] Remote `torrent-add` (magnet, .torrent file, URL)
- [x] Remote torrent actions (start/stop/verify/remove via RPC; list refreshes after mutations)
- [x] `session-stats` polling for status bar
- [x] Copy magnet link, alt-speed toggle via `session-set`
- [x] Remote context menu actions (start/stop/queue/verify/reannounce/remove, copy magnet, open folder, set location)
- [x] Properties dialog in remote mode (info, options, peers, trackers via RPC)
- [x] Stats dialog via session-stats RPC
- [x] Remote feature parity: session-get/set sync, expanded list poll, status bar speeds, filter by tracker, properties files/trackers, blocklist, options on add
- [x] Preferences → Remote: session mode dropdown (Act as remote / Act as host / None) with context-specific panels
