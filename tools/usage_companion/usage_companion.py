#!/usr/bin/env python3
"""
Claude Code Usage Companion — pushes session/weekly token-% to the M5Stack buddy over BLE.

The device firmware parses {"session_pct":N,"weekly_pct":M} and shows S__% W__% in the
status bar. This script reads ~/.claude/projects/**/*.jsonl, computes rolling 5h/7d
output-token windows, and writes to the device over BLE every 20s.

── macOS Bluetooth permission ──────────────────────────────────────────────────────────
Python scripts SIGABRT on macOS without Bluetooth permission. Fix once:
  System Settings → Privacy & Security → Bluetooth → add your Terminal (or iTerm).
After granting, run this script directly — no app bundle needed:
  .venv/bin/python tools/usage_companion/usage_companion.py

── Firmware compatibility ───────────────────────────────────────────────────────────────
  New firmware (after 2026-05-31): uses plaintext char 6e400004 — no pairing needed.
  Old firmware: falls back to encrypted NUS RX 6e400002 (macOS uses stored bond LTK).
The script auto-detects which one to use.

── Cap tuning ───────────────────────────────────────────────────────────────────────────
  SESSION_CAP  output tokens / 5h window  (default 88000, Claude Code Pro Max approx.)
  WEEKLY_CAP   output tokens / 7d window  (default 500000)
  SESSION_CAP=200000 WEEKLY_CAP=1000000 .venv/bin/python usage_companion.py

Requires: pip install bleak
"""

import asyncio
import glob
import json
import os
import sys
from datetime import datetime, timezone, timedelta

from bleak import BleakClient, BleakScanner

# ── NUS service / characteristic UUIDs ─────────────────────────────────────────
NUS_SERVICE  = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX       = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # encrypted (old firmware + new)
NUS_USAGE_RX = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  # plaintext (new firmware only)

# ── Configurable caps ───────────────────────────────────────────────────────────
SESSION_HOURS = 5
WEEKLY_DAYS   = 7
SESSION_CAP   = int(os.environ.get("SESSION_CAP", "88000"))
WEEKLY_CAP    = int(os.environ.get("WEEKLY_CAP",  "500000"))

PUSH_INTERVAL = 20   # seconds between BLE writes
BUDDY_PREFIX  = "claude"


# ── Token accounting ────────────────────────────────────────────────────────────

def _collect_tokens():
    """Scan all ~/.claude/projects/**/*.jsonl → list of (utc_datetime, output_tokens)."""
    records = []
    for path in glob.glob(os.path.expanduser("~/.claude/projects/**/*.jsonl"), recursive=True):
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        d = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if d.get("type") != "assistant":
                        continue
                    msg = d.get("message")
                    if not isinstance(msg, dict):
                        continue
                    usage = msg.get("usage")
                    if not isinstance(usage, dict):
                        continue
                    out_tok = usage.get("output_tokens", 0)
                    if not out_tok:
                        continue
                    ts_str = d.get("timestamp", "")
                    if not ts_str:
                        continue
                    try:
                        ts = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
                    except ValueError:
                        continue
                    records.append((ts, out_tok))
        except OSError:
            continue
    return records


def compute_usage_pct() -> tuple[int, int]:
    """Return (session_pct, weekly_pct) clamped to 0-100."""
    now = datetime.now(timezone.utc)
    cutoff_5h  = now - timedelta(hours=SESSION_HOURS)
    cutoff_7d  = now - timedelta(days=WEEKLY_DAYS)
    s = w = 0
    for ts, tok in _collect_tokens():
        if ts >= cutoff_5h:
            s += tok
        if ts >= cutoff_7d:
            w += tok
    return min(100, int(s * 100 / SESSION_CAP)), min(100, int(w * 100 / WEEKLY_CAP))


# ── BLE helpers ─────────────────────────────────────────────────────────────────

async def find_buddy(timeout: float = 12.0) -> str | None:
    print(f"[companion] scanning {timeout:.0f}s for 'Claude*' device …", flush=True)
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for addr, (dev, adv) in devices.items():
        name = (adv.local_name or dev.name or "").lower()
        if name.startswith(BUDDY_PREFIX):
            label = adv.local_name or dev.name
            print(f"[companion] found: {label!r} at {addr}", flush=True)
            return addr
    return None


def _pick_write_char(client: BleakClient) -> str | None:
    """Return the UUID to write usage data to, preferring the plaintext char."""
    uuids = {str(c.uuid).lower() for s in client.services for c in s.characteristics}
    if NUS_USAGE_RX in uuids:
        return NUS_USAGE_RX   # new firmware — plaintext, no bonding needed
    if NUS_RX in uuids:
        return NUS_RX         # old firmware — macOS uses stored bond LTK automatically
    return None


async def push_loop():
    while True:
        addr = await find_buddy()
        if addr is None:
            print("[companion] no buddy found — retrying in 30s", flush=True)
            await asyncio.sleep(30)
            continue

        print(f"[companion] connecting to {addr} …", flush=True)
        try:
            async with BleakClient(addr, timeout=15.0) as client:
                char_uuid = _pick_write_char(client)
                if char_uuid is None:
                    print("[companion] ERROR: NUS service not found on device. "
                          "Is it the right device?", flush=True)
                    await asyncio.sleep(10)
                    continue
                kind = "plaintext" if char_uuid == NUS_USAGE_RX else "encrypted (bond LTK)"
                print(f"[companion] using char {char_uuid} ({kind})", flush=True)
                print(f"[companion] connected — pushing every {PUSH_INTERVAL}s  "
                      f"(Ctrl-C to stop)", flush=True)

                while client.is_connected:
                    sp, wk = compute_usage_pct()
                    payload = json.dumps({"session_pct": sp, "weekly_pct": wk}) + "\n"
                    print(f"[companion] → {payload.strip()}", flush=True)
                    try:
                        await client.write_gatt_char(
                            char_uuid,
                            payload.encode("utf-8"),
                            response=False,
                        )
                    except Exception as write_err:
                        print(f"[companion] write failed: {write_err}", flush=True)
                        if char_uuid == NUS_RX:
                            print("[companion] hint: if this is a bonding error, "
                                  "pair the device with Hardware Buddy first.", flush=True)
                        break
                    await asyncio.sleep(PUSH_INTERVAL)
        except Exception as exc:
            print(f"[companion] connection error ({exc}) — reconnecting in 5s", flush=True)
            await asyncio.sleep(5)


def main():
    if "--dry-run" in sys.argv:
        print("[companion] DRY RUN — computing usage without BLE")
        sp, wk = compute_usage_pct()
        print(f"  session_pct = {sp}%  (5h window, cap={SESSION_CAP} tokens)")
        print(f"  weekly_pct  = {wk}%  (7d window, cap={WEEKLY_CAP} tokens)")
        print("Set SESSION_CAP / WEEKLY_CAP env vars to tune for your plan.")
        return

    print(f"[companion] SESSION_CAP={SESSION_CAP}  WEEKLY_CAP={WEEKLY_CAP}")
    print("[companion] If this SIGABRTs: System Settings → Privacy & Security → "
          "Bluetooth → add your Terminal app.")
    try:
        asyncio.run(push_loop())
    except KeyboardInterrupt:
        print("\n[companion] stopped")


if __name__ == "__main__":
    main()
