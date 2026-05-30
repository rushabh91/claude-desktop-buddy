# Usage Companion

Pushes Claude Code token-usage percentages to the M5Stack Fire buddy over BLE every 20s.
The device status bar shows `S__% W__%` (session / weekly) colored green/yellow/red.

## Setup

```bash
# From the repo root:
python3 -m venv .venv
.venv/bin/pip install bleak
```

### macOS Bluetooth permission (one-time)

Python SIGABRTs if it doesn't have Bluetooth access. Grant it once:

> **System Settings → Privacy & Security → Bluetooth → "+" → add your Terminal app**
> (Terminal.app, iTerm2, Warp, etc.)

After granting, the script runs directly from the terminal with no app bundle needed.

## Running

```bash
# From the repo root:
.venv/bin/python tools/usage_companion/usage_companion.py
```

Dry-run (prints percentages, no BLE write — useful for checking token counts):

```bash
.venv/bin/python tools/usage_companion/usage_companion.py --dry-run
```

## Firmware compatibility

| Firmware | Characteristic used | Notes |
|---|---|---|
| New (≥ 2026-05-31) | `6e400004` (plaintext) | No pairing needed |
| Old | `6e400002` (encrypted) | Uses macOS stored bond LTK; pair with Hardware Buddy first |

The companion auto-detects which characteristic the device has.

## Cap tuning

The percentages are `(output_tokens_in_window / cap) × 100`. Default caps:

| Variable | Default | Meaning |
|---|---|---|
| `SESSION_CAP` | 88000 | Output tokens / 5-hour window |
| `WEEKLY_CAP` | 500000 | Output tokens / 7-day window |

These are rough approximations for Claude Code Pro Max. Adjust to match your plan:

```bash
SESSION_CAP=200000 WEEKLY_CAP=1000000 .venv/bin/python tools/usage_companion/usage_companion.py
```

## How it works

1. Reads every `~/.claude/projects/**/*.jsonl` file
2. Sums `output_tokens` from `assistant` entries in the last 5h and 7d
3. Divides by cap → 0–100%
4. Connects to any BLE device named `Claude*`
5. Auto-detects the write characteristic (plaintext `6e400004` or encrypted `6e400002`)
6. Writes `{"session_pct":N,"weekly_pct":M}\n` every 20s; reconnects on drop
