# transmission-client-gtk

A fork of [Transmission](https://github.com/transmission/transmission) focused on one goal: **the GTK client can act as a remote control for an existing `transmission-daemon`**, the way `transmission-qt` already does.

Upstream `transmission-gtk` always embeds its own BitTorrent session. This build adds a full HTTP RPC path so you can manage the **same daemon** already in use, without switching to Qt or the not so good looking remote-only app.

**Details:** [gtk/README.md](gtk/README.md) (config, build, feature list)

## What makes this version special

| | **Stock `transmission-gtk`** | **`transmission-client-gtk` (this fork)** |
|---|------------------------------|-------------------------------------------|
| Default session | Embedded in-process daemon | Connect to a remote daemon over RPC |
| Same torrents as `transmission-daemon` | No—separate config under `~/.config/transmission/` | Yes—when set to remote mode |
| `remote-session-*` settings in `settings.json` | Ignored (Qt-only keys today) | Honored; drives connection |
| RPC when not hosting | Errors: *"GTK+ client doesn't support connections to remote servers yet"* | `RpcClient` + remote code path in `Session` |
| UI | Standard GTK shell | Same look and menus, wired for remote torrent/session ops |
| Binary / config dir | `transmission-gtk` / `~/.config/transmission/` | `transmission-client-gtk` / `~/.config/transmission-client-gtk/` |

**Why it exists:** On Linux, many setups run `transmission-daemon` as a system service (VPN rules, fixed download dir, automation). Stock GTK cannot attach to that daemon; Qt can, but you may prefer GTK. This fork ports the Qt remote model into the GTK codebase (libsoup RPC, polling, remote prefs) so one familiar UI controls the real server.

**Session modes** (Preferences → Remote): **Act as remote** (daemon client), **Act as host** (embedded + RPC), or **None** (local only). Remote mode requires an app restart after changing mode.

## Quick start (remote mode)

1. Build `transmission-client-gtk` (see [gtk/README.md](gtk/README.md)).
2. Ensure your daemon’s RPC is reachable (e.g. `127.0.0.1:9091`).
3. Set `~/.config/transmission-client-gtk/settings.json`:

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

4. Launch `transmission-client-gtk` — torrents and paths should match the daemon, not a stray local session.

## Upstream

Based on Transmission at `56442e2`. Licensed under the same terms as upstream (GPL). Not affiliated with the Transmission project.

For stock Transmission docs, building all targets, and contributing upstream, see [transmission/transmission](https://github.com/transmission/transmission) and `docs/`.
