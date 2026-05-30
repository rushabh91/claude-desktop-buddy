# Usage Companion

Pushes Claude Code token-usage percentages to the M5Stack Fire buddy over BLE every 20s.
The device status bar shows `S__% W__%` (session / weekly) colored green/yellow/red.

## Setup

```bash
# From the repo root:
python3 -m venv .venv
.venv/bin/pip install bleak
```

## Running

macOS requires a proper app bundle for Bluetooth access (bare Python SIGABRTs):

```bash
open tools/usage_companion/UsageCompanion.app
```

For dry-run (prints percentages without BLE):

```bash
.venv/bin/python tools/usage_companion/usage_companion.py --dry-run
```

## Cap tuning

The percentages are `(output_tokens_in_window / cap) * 100`. Default caps:

| Variable      | Default | What it represents                        |
|---------------|---------|-------------------------------------------|
| `SESSION_CAP` | 88000   | Output tokens allowed in a 5-hour window  |
| `WEEKLY_CAP`  | 500000  | Output tokens allowed in a 7-day window   |

These are approximate for the Claude Code Pro Max plan. Adjust for your plan:

```bash
SESSION_CAP=200000 WEEKLY_CAP=1000000 open tools/usage_companion/UsageCompanion.app
```

## How it works

1. Reads every `~/.claude/projects/**/*.jsonl` file
2. Sums `output_tokens` from `assistant` entries in the last 5h (session) and 7d (weekly)
3. Divides by cap → 0–100%
4. Connects to any BLE device named `Claude*` (NUS service `6e400001-…`)
5. Writes `{"session_pct":N,"weekly_pct":M}\n` to the NUS RX characteristic (`6e400002-…`)
6. Reconnects automatically on drop

## BLE device address

The companion connects to the first `Claude*` device it finds. If you have multiple buddies,
set the target by editing `BUDDY_PREFIX` in `usage_companion.py` to match the full name
(e.g. `"Claude-B21E"`).
