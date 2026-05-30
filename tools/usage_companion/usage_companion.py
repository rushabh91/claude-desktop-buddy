#!/usr/bin/env python3
"""
Claude Code Usage Companion — pushes real session/weekly usage % to the M5Stack buddy over BLE.

The numbers come from the **official Anthropic usage API** (the same source the Claude Code
statusline and tools like claude-hud use), NOT from estimating token counts:

    GET https://api.anthropic.com/api/oauth/usage
    → { "five_hour": {"utilization": 0-100, "resets_at": ...},
        "seven_day": {"utilization": 0-100, "resets_at": ...} }

`utilization` is the true percent-of-limit used. The OAuth bearer token is read from your
local Claude Code credentials (macOS Keychain "Claude Code-credentials", or
~/.claude/.credentials.json) — exactly the credential Claude Code itself stores.

The companion pushes {"session_pct":N,"weekly_pct":M} to the device every 20s; the firmware
shows it as S__% W__% in the status bar.

── macOS Bluetooth + Keychain permissions ──────────────────────────────────────────────
  • Bluetooth: System Settings → Privacy & Security → Bluetooth → add your Terminal app.
  • Keychain: the first run prompts to read "Claude Code-credentials" — click Always Allow.

── Usage ────────────────────────────────────────────────────────────────────────────────
  .venv/bin/python tools/usage_companion/usage_companion.py            # run + push over BLE
  .venv/bin/python tools/usage_companion/usage_companion.py --dry-run  # print %, no BLE

Requires: pip install bleak   (the API call uses only the Python stdlib)
"""

import asyncio
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

from bleak import BleakClient, BleakScanner

# ── NUS service / characteristic UUIDs ─────────────────────────────────────────
NUS_USAGE_RX = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  # plaintext (new firmware)
NUS_RX       = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # encrypted (old firmware fallback)

# ── Anthropic usage API (same request claude-hud / Claude Code statusline make) ─
USAGE_API_URL    = "https://api.anthropic.com/api/oauth/usage"
USAGE_API_BETA   = "oauth-2025-04-20"
USAGE_USER_AGENT = "claude-code/2.1"
KEYCHAIN_SERVICE = "Claude Code-credentials"

PUSH_INTERVAL = 20    # seconds between BLE writes
TOKEN_TTL     = 240   # re-read the keychain token at most every 4 min
BUDDY_PREFIX  = "claude"


# ── Credentials ───────────────────────────────────────────────────────────────

def _read_keychain_token() -> str | None:
    """macOS Keychain: the OAuth blob Claude Code 2.x stores under 'Claude Code-credentials'."""
    if sys.platform != "darwin":
        return None
    user = os.environ.get("USER") or ""
    # Try with account name first (how Claude Code writes it), then generic.
    for args in (
        ["-s", KEYCHAIN_SERVICE, "-a", user, "-w"] if user else None,
        ["-s", KEYCHAIN_SERVICE, "-w"],
    ):
        if args is None:
            continue
        try:
            out = subprocess.run(
                ["/usr/bin/security", "find-generic-password", *args],
                capture_output=True, text=True, timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        if out.returncode != 0 or not out.stdout.strip():
            continue
        tok = _extract_token(out.stdout.strip())
        if tok:
            return tok
    return None


def _read_file_token() -> str | None:
    """Older / non-macOS: ~/.claude/.credentials.json."""
    path = os.path.expanduser(
        os.path.join(os.environ.get("CLAUDE_CONFIG_DIR", "~/.claude"), ".credentials.json")
    )
    try:
        with open(path, encoding="utf-8") as fh:
            return _extract_token(fh.read())
    except OSError:
        return None


def _extract_token(raw: str) -> str | None:
    """Pull a non-expired accessToken out of a claudeAiOauth credential blob."""
    try:
        oauth = json.loads(raw).get("claudeAiOauth") or {}
    except (json.JSONDecodeError, AttributeError):
        return None
    token = oauth.get("accessToken")
    if not token:
        return None
    expires_at = oauth.get("expiresAt")  # unix ms
    if isinstance(expires_at, (int, float)):
        import time
        if expires_at <= time.time() * 1000:
            return None  # expired — Claude Code will refresh it; we retry next cycle
    return token


def get_access_token() -> str | None:
    return _read_keychain_token() or _read_file_token()


# ── Usage API ─────────────────────────────────────────────────────────────────

def _clamp_pct(value) -> int | None:
    if value is None or not isinstance(value, (int, float)):
        return None
    try:
        return round(max(0, min(100, value)))
    except (ValueError, TypeError):
        return None


def fetch_usage(token: str) -> tuple[int | None, int | None]:
    """Return (session_pct, weekly_pct) from the OAuth usage API, or (None, None) on failure."""
    req = urllib.request.Request(
        USAGE_API_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": USAGE_API_BETA,
            "User-Agent": USAGE_USER_AGENT,
        },
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            if resp.status != 200:
                print(f"[companion] usage API HTTP {resp.status}", flush=True)
                return None, None
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 401:
            print("[companion] usage API 401 — token expired, will re-read keychain", flush=True)
        else:
            print(f"[companion] usage API HTTP {e.code}", flush=True)
        return None, None
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as e:
        print(f"[companion] usage API error: {e}", flush=True)
        return None, None

    session = _clamp_pct((data.get("five_hour") or {}).get("utilization"))
    weekly  = _clamp_pct((data.get("seven_day") or {}).get("utilization"))
    return session, weekly


# ── Token caching wrapper ───────────────────────────────────────────────────────

class _TokenCache:
    def __init__(self):
        self._token = None
        self._at = 0.0

    def get(self, force_refresh: bool = False) -> str | None:
        import time
        if force_refresh or self._token is None or (time.time() - self._at) > TOKEN_TTL:
            tok = get_access_token()
            if tok:
                self._token, self._at = tok, time.time()
            elif force_refresh:
                self._token = None
        return self._token


_tokens = _TokenCache()


def compute_usage_pct(force_token_refresh: bool = False) -> tuple[int | None, int | None]:
    token = _tokens.get(force_refresh=force_token_refresh)
    if not token:
        print("[companion] no Claude Code OAuth token found — is Claude Code logged in?",
              flush=True)
        return None, None
    s, w = fetch_usage(token)
    if s is None and w is None and not force_token_refresh:
        # Maybe the token just expired — force a re-read and try once more.
        return compute_usage_pct(force_token_refresh=True)
    return s, w


# ── BLE ──────────────────────────────────────────────────────────────────────

async def find_buddy(timeout: float = 12.0) -> str | None:
    print(f"[companion] scanning {timeout:.0f}s for 'Claude*' device …", flush=True)
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for addr, (dev, adv) in devices.items():
        name = (adv.local_name or dev.name or "").lower()
        if name.startswith(BUDDY_PREFIX):
            print(f"[companion] found: {adv.local_name or dev.name!r} at {addr}", flush=True)
            return addr
    return None


def _pick_write_char(client: BleakClient) -> str | None:
    uuids = {str(c.uuid).lower() for s in client.services for c in s.characteristics}
    if NUS_USAGE_RX in uuids:
        return NUS_USAGE_RX
    if NUS_RX in uuids:
        return NUS_RX
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
                    print("[companion] ERROR: NUS service not found on device.", flush=True)
                    await asyncio.sleep(10)
                    continue
                kind = "plaintext" if char_uuid == NUS_USAGE_RX else "encrypted (bond LTK)"
                print(f"[companion] connected — char {char_uuid} ({kind}), "
                      f"pushing every {PUSH_INTERVAL}s  (Ctrl-C to stop)", flush=True)
                while client.is_connected:
                    s, w = compute_usage_pct()
                    # Send only the fields we actually have; firmware leaves the rest at -1.
                    payload_obj = {}
                    if s is not None:
                        payload_obj["session_pct"] = s
                    if w is not None:
                        payload_obj["weekly_pct"] = w
                    if payload_obj:
                        payload = json.dumps(payload_obj) + "\n"
                        print(f"[companion] → {payload.strip()}", flush=True)
                        try:
                            await client.write_gatt_char(char_uuid, payload.encode(), response=False)
                        except Exception as we:
                            print(f"[companion] write failed: {we}", flush=True)
                            break
                    else:
                        print("[companion] no usage data this cycle (skipping push)", flush=True)
                    await asyncio.sleep(PUSH_INTERVAL)
        except Exception as exc:
            print(f"[companion] connection error ({exc}) — reconnecting in 5s", flush=True)
            await asyncio.sleep(5)


def main():
    if "--dry-run" in sys.argv:
        print("[companion] DRY RUN — querying real Anthropic usage API (no BLE)")
        s, w = compute_usage_pct()
        if s is None and w is None:
            print("  Could not read usage. Is Claude Code logged in on this machine?")
            print("  (First run prompts for Keychain access — click Always Allow.)")
        else:
            print(f"  session_pct (5h)  = {s}%")
            print(f"  weekly_pct  (7d)  = {w}%")
        return

    print("[companion] using the official Anthropic usage API for real percentages")
    print("[companion] if this hangs on first run, approve the Keychain prompt (Always Allow)")
    try:
        asyncio.run(push_loop())
    except KeyboardInterrupt:
        print("\n[companion] stopped")


if __name__ == "__main__":
    main()
