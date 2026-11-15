# MCP server (AI assistants)

`tools/mcp_slink.py` is a [Model Context Protocol](https://modelcontextprotocol.io) server for the S-Link bridge: an AI assistant (Claude Desktop, Claude Code, Cursor, any MCP client) can read the state of the Sony amplifier and control it ("switch it on, select the tuner, put on Radio 2, quiet volume") through the bridge's web portal.

It runs **on a computer**, not on the ESP32: MCP clients start it as a local program and it talks to the bridge over the local network (HTTP + WebSocket, the same ones the web page uses). Only the Python standard library is needed (Python 3.8 or newer), so there is nothing to install: copy `tools/mcp_slink.py` anywhere. The web portal of the bridge must be on (`WEB ON`).

## Connecting a client

**Claude Code**

```
claude mcp add slink -- python /path/to/tools/mcp_slink.py --host slink.local
```

**Claude Desktop** (`claude_desktop_config.json`, *Settings › Developer › Edit config*)

```json
{
  "mcpServers": {
    "slink": {
      "command": "python",
      "args": ["C:/path/to/tools/mcp_slink.py", "--host", "slink.local"],
      "env": { "SLINK_MAX_VOLUME": "25" }
    }
  }
}
```

Other clients: the same command in their MCP settings (transport *stdio*). If `slink.local` does not resolve on that computer, use the IP address. With the bridge's HTTPS: `--tls --cafile slink-ca.crt` (the CA downloaded from `/ca.crt`).

**A client on another machine** (Streamable HTTP): `python tools/mcp_slink.py --http 8765 --bind 0.0.0.0 --token SECRET` and point the client at `http://<this computer>:8765/mcp` with `Authorization: Bearer SECRET`. Without `--bind` it listens on 127.0.0.1 only; binding to a network address *requires* `--token`; requests with a foreign `Origin` header are refused. There is no TLS: keep it on the local network.

## What the assistant gets

**Tools** (all `openWorldHint: false`; the read-only ones are marked `readOnlyHint`)

| Tool | What it does |
|---|---|
| `get_status` | power, mute, input, volume estimate, sound field, tuner (band, frequency, RDS name, preset), input labels |
| `get_radio` | the tuner now, the amplifier presets (3 × 10), the FM stations and favourites kept by the bridge |
| `get_device_info` | firmware, uptime, memory, WiFi, Bluetooth, S-Link bus counters |
| `refresh_status` | ask the units on the bus again (`STATUS`) and return the new status |
| `power` | on / standby |
| `set_volume`, `change_volume` | volume in % of the knob travel; steps up / down. **Capped** (see below) |
| `mute` | mute / unmute |
| `select_input` | TUNER PHONO CD MD DAT TAPE VIDEO1-3 DVD SAT TV |
| `tune`, `tune_station`, `recall_preset`, `seek` | FM/AM frequency, a stored station by name or `#n`, an amplifier preset, scan up / down |
| `set_sound_field`, `get_eq`, `set_eq` | sound field 0-29; the 3-band equaliser |
| `all_off` | power off every Sony unit on the bus |
| `console_command` | any other console command (`PRESETS 1`, `FAV`, `EQCURVE` ...) and its output; network, security, firmware and volume commands are refused |
| `send_raw_frame` | a raw S-Link frame. **Off** unless `--allow-raw` |
| `recent_log` | the last lines the bridge wrote |

**Resources**: `slink://state`, `slink://radio/presets`, `slink://radio/stations`, `slink://radio/favorites`, `slink://device/stats`, `slink://device/help`, `slink://log`. In stdio mode a client can subscribe to `slink://state` and is notified when the amplifier changes.

**Prompts**: `radio_evening` (power on, tuner, a station, moderate volume), `check_system` (health report), `safe_listening` (bring the volume to a quiet level).

## Safety

The bridge has no login, and the one thing an assistant must not cause is a loud sound:

- the volume never goes above `--max-volume` (default **30**, percent of the knob travel; `$SLINK_MAX_VOLUME`), whatever is asked. Going up by steps is also refused when the knob position is not known. Volume tools refuse while the amplifier is in standby (the knob does not move). `VOL ...` is not accepted by `console_command`;
- `console_command` refuses `WIFI`, `OTA`, `CFG`, `MQTT`, `HTTPS`, `HUE`, `BLE`, `WEB`, `APP`, `HOSTNAME`, `VOL` and the other commands that change the network, the security, the firmware or the volume (`--allow-admin` lifts it); raw hex frames need `--allow-raw`;
- `--dry-run` writes the commands to the log instead of sending them: a way to see what an assistant would do.

Options can also be set with `$SLINK_HOST`, `$SLINK_TLS=1`, `$SLINK_CA`, `$SLINK_MCP_TOKEN`, `$SLINK_MCP_RAW=1`, `$SLINK_MCP_ADMIN=1`, `$SLINK_DRY_RUN=1`.

## How it works

The server speaks MCP 2025-06-18 (2025-03-26 and 2024-11-05 are accepted): JSON-RPC over stdio, one message per line, or POSTed to `/mcp` in the HTTP mode (answers are plain JSON, no SSE). Readings use the portal's REST calls (`/api/state`, `/api/presets`, `/api/stations`, `/api/favs`, `/api/stats`); commands go over the portal's WebSocket, which also returns the console output (`OK`, `ERR`, the decoded `RX` frames) that the tools put in their result. One command runs at a time.

Checked against the real bridge: the protocol exchange (initialize, tools, resources, prompts, errors), the read tools, every refusal, the command each write tool builds (with `--dry-run`), `STATUS` and `PRESETS`, and the HTTP mode with token and origin checks. Not tried: a real MCP client (Claude Desktop / Code), and the write tools on a live amplifier.
