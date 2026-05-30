#!/usr/bin/env python3
"""
Claude Code Usage Companion — pushes session/weekly token-% to the M5Stack buddy over BLE.

The device firmware (Stage 1) already parses {"session_pct":N,"weekly_pct":M} and displays
it in the status-bar center. This script reads ~/.claude/projects/**/*.jsonl, computes rolling
5-hour and 7-day output-token windows, converts to %, and writes to the NUS RX characteristic.

macOS Bluetooth restriction: Python scripts SIGABRT without NSBluetoothAlwaysUsageDescription.
Run via `open UsageCompanion.app` instead of calling this script directly.

Usage:
    SESSION_CAP=88000 WEEKLY_CAP=500000 .venv/bin/python usage_companion.py [--dry-run]
    # Or via the app bundle (handles entitlement):
    open tools/usage_companion/UsageCompanion.app

Cap tuning:
    SESSION_CAP  = output tokens allowed in a rolling 5-hour window  (default: 88000)
    WEEKLY_CAP   = output tokens allowed in a rolling 7-day window   (default: 500000)
    These are approximations for Claude Code Pro Max; adjust to match your plan's actual limits.
    The displayed % is relative — green <70%, yellow 70-89%, red >=90%.

Requires: bleak (pip install bleak)
"""

import asyncio
import glob
import json
import os
import sys
import time
from datetime import datetime, timezone, timedelta

from bleak import BleakClient, BleakScanner

# ── NUS service / characteristic UUIDs (same as firmware) ──────────────────────
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write here → device receives

# ── Configurable caps (tune to your subscription plan) ─────────────────────────
SESSION_HOURS = 5
WEEKLY_DAYS   = 7
SESSION_CAP   = int(os.environ.get("SESSION_CAP", "88000"))   # output tokens / 5h
WEEKLY_CAP    = int(os.environ.get("WEEKLY_CAP",  "500000"))  # output tokens / 7d

PUSH_INTERVAL = 20   # seconds between BLE writes
BUDDY_PREFIX  = "claude"  # case-insensitive name prefix for the device


# ── Token accounting ────────────────────────────────────────────────────────────

def _collect_tokens():
    """Scan all ~/.claude/projects/**/*.jsonl and return list of (utc_ts, output_tokens)."""
    pattern = os.path.expanduser("~/.claude/projects/**/*.jsonl")
    records = []
    for path in glob.glob(pattern, recursive=True):
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
    """Return (session_pct, weekly_pct) as integers 0-100."""
    now = datetime.now(timezone.utc)
    cutoff_session = now - timedelta(hours=SESSION_HOURS)
    cutoff_weekly  = now - timedelta(days=WEEKLY_DAYS)

    session_tokens = 0
    weekly_tokens  = 0
    for ts, out_tok in _collect_tokens():
        if ts >= cutoff_session:
            session_tokens += out_tok
        if ts >= cutoff_weekly:
            weekly_tokens += out_tok

    session_pct = min(100, int(session_tokens * 100 / SESSION_CAP))
    weekly_pct  = min(100, int(weekly_tokens  * 100 / WEEKLY_CAP))
    return session_pct, weekly_pct


# ── BLE push loop ───────────────────────────────────────────────────────────────

async def find_buddy(timeout: float = 12.0) -> str | None:
    """Scan for the first device whose name starts with 'Claude' (case-insensitive)."""
    print(f"[companion] scanning {timeout:.0f}s for device name starting with '{BUDDY_PREFIX}' …")
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for addr, (dev, adv) in devices.items():
        name = (adv.local_name or dev.name or "").lower()
        if name.startswith(BUDDY_PREFIX):
            print(f"[companion] found: {adv.local_name or dev.name!r} at {addr}")
            return addr
    return None


async def push_loop(dry_run: bool = False):
    while True:
        addr = await find_buddy()
        if addr is None:
            print("[companion] no buddy found — retrying in 30s")
            await asyncio.sleep(30)
            continue

        print(f"[companion] connecting to {addr} …")
        try:
            async with BleakClient(addr, timeout=15.0) as client:
                print(f"[companion] connected — pushing every {PUSH_INTERVAL}s  (Ctrl-C to stop)")
                while client.is_connected:
                    sp, wk = compute_usage_pct()
                    payload = json.dumps({"session_pct": sp, "weekly_pct": wk}) + "\n"
                    print(f"[companion] → {payload.strip()}")
                    if not dry_run:
                        await client.write_gatt_char(
                            NUS_RX,
                            payload.encode("utf-8"),
                            response=False,
                        )
                    await asyncio.sleep(PUSH_INTERVAL)
        except Exception as exc:
            print(f"[companion] disconnected ({exc}) — reconnecting in 5s")
            await asyncio.sleep(5)


def main():
    dry_run = "--dry-run" in sys.argv
    if dry_run:
        print("[companion] DRY RUN — computing usage without BLE write")
        sp, wk = compute_usage_pct()
        print(f"[companion] session_pct={sp}  weekly_pct={wk}")
        print(f"[companion] (SESSION_CAP={SESSION_CAP}, WEEKLY_CAP={WEEKLY_CAP})")
        return

    try:
        asyncio.run(push_loop())
    except KeyboardInterrupt:
        print("\n[companion] stopped")


if __name__ == "__main__":
    main()
