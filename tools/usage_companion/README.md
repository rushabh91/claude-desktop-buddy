# Usage Companion

Pushes **real** Claude Code usage percentages to the M5Stack Fire buddy over BLE every 5 min
(the API's refresh window). The device status bar shows `S__% W__%` (session / weekly)
colored green/yellow/red.

The numbers come from the **official Anthropic usage API** — the same source the Claude Code
statusline and tools like [claude-hud](https://github.com/jarrodwatts/claude-hud) use:

```
GET https://api.anthropic.com/api/oauth/usage
→ { "five_hour": {"utilization": 0-100}, "seven_day": {"utilization": 0-100} }
```

`utilization` is the true percent-of-limit used — no token estimation or guessed caps.
The OAuth token is read from your local Claude Code credentials (macOS Keychain
`Claude Code-credentials`, falling back to `~/.claude/.credentials.json`).

## Setup

```bash
# From the repo root:
python3 -m venv .venv
.venv/bin/pip install bleak    # the API call itself uses only the stdlib
```

### One-time macOS permissions

1. **Bluetooth:** System Settings → Privacy & Security → Bluetooth → add your Terminal
   app (Terminal.app, iTerm2, Warp, …). Without this, Python SIGABRTs when it touches BLE.
2. **Keychain:** the first run prompts to read `Claude Code-credentials` — click
   **Always Allow**.

## Running

```bash
# Verify the numbers first (no BLE, just prints real %):
.venv/bin/python tools/usage_companion/usage_companion.py --dry-run

# Then run for real (scans, connects, pushes every 5 min):
.venv/bin/python tools/usage_companion/usage_companion.py
```

## Firmware compatibility

| Firmware | Characteristic | Notes |
|---|---|---|
| New (≥ 2026-05-31) | `6e400004` (plaintext) | No pairing needed |
| Old | `6e400002` (encrypted) | Uses macOS stored bond LTK; pair with Hardware Buddy first |

The companion auto-detects which characteristic the device exposes.

## How it works

1. Reads the Claude Code OAuth token from the Keychain (or `.credentials.json`)
2. `GET /api/oauth/usage` → `five_hour`/`seven_day` `utilization` + `resets_at`
3. Connects to any BLE device named `Claude*`
4. Writes a compact JSON payload every 5 min; reconnects on drop:
   `{"session_pct":N,"weekly_pct":M,"session_reset":secs,"weekly_reset":secs,"rate_limited":bool}`
   (`*_reset` = seconds until that window resets; `rate_limited` only sent when true)
5. If the token is stale, nudges `claude auth status` so the CLI refreshes its own keychain
   item, then retries (we never mint/write tokens ourselves)
6. **Fallback:** if `/api/oauth/usage` fails, makes a 1-token `/v1/messages` call and reads the
   `anthropic-ratelimit-unified-*` response headers (Clawdmeter's approach)

The device shows session/weekly % in the status bar (with a red `!` when rate-limited) and the
full percentages + reset countdowns on the **info screen** (hold/press to the CLAUDE page).

## Troubleshooting

- **`no Claude Code OAuth token found`** — make sure Claude Code is logged in on this
  machine (`claude` CLI or the desktop app, signed into your Anthropic account).
- **Hangs on first run** — approve the Keychain access prompt.
- **API users (no subscription)** — the usage API only returns data for Pro/Max/Team
  plans; raw-API-key users have no rolling-window limits to report.
