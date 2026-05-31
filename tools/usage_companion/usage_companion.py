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

The companion polls the API and pushes {"session_pct":N,"weekly_pct":M} to the device every
5 minutes (the API's refresh window); the firmware shows it as S__% W__% in the status bar.

── macOS Bluetooth + Keychain permissions ──────────────────────────────────────────────
  • Bluetooth: System Settings → Privacy & Security → Bluetooth → add your Terminal app.
  • Keychain: the first run prompts to read "Claude Code-credentials" — click Always Allow.

── Usage ────────────────────────────────────────────────────────────────────────────────
  .venv/bin/python tools/usage_companion/usage_companion.py            # run + push over BLE
  .venv/bin/python tools/usage_companion/usage_companion.py --dry-run  # print %, no BLE

Requires: pip install bleak   (the API call uses only the Python stdlib)
"""

import asyncio
import glob
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone

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

# Fallback source (Clawdmeter-style): a 1-token /v1/messages call whose response
# carries anthropic-ratelimit-unified-* headers. Only used if /api/oauth/usage fails.
MESSAGES_API_URL = "https://api.anthropic.com/v1/messages"
ANTHROPIC_VERSION = "2023-06-01"

# The usage API only refreshes ~every 5 min (its rate-limit window), so polling
# faster just burns API calls for no new data. One push per 5 min is plenty.
PUSH_INTERVAL = 300   # seconds between API polls + BLE writes
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


# ── CLI token-refresh nudge ─────────────────────────────────────────────────────
# Claude Code's OAuth token lasts ~8-12h and the `claude` CLI refreshes it on its
# own when it expires. We don't mint/write tokens ourselves (risky + the credential
# is owned by Claude Code). Instead, when our read finds no valid token, we run a
# cheap `claude auth status` so the CLI can refresh its own keychain item, then
# re-read. Best-effort: if it doesn't help, we just skip the cycle.

_claude_bin: str | None = None


def _find_claude_bin() -> str | None:
    if os.environ.get("CLAUDE_BIN"):
        return os.environ["CLAUDE_BIN"]
    found = shutil.which("claude")
    if found:
        return found
    # launchd has a minimal PATH — search common dirs + nvm installs.
    cands = [os.path.expanduser("~/.local/bin/claude"), "/opt/homebrew/bin/claude",
             "/usr/local/bin/claude"]
    cands += sorted(glob.glob(os.path.expanduser("~/.nvm/versions/node/*/bin/claude")))
    for c in cands:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def _nudge_cli_refresh() -> None:
    global _claude_bin
    if _claude_bin is None:
        _claude_bin = _find_claude_bin() or ""
    if not _claude_bin:
        print("[companion] `claude` CLI not found for token refresh "
              "(set CLAUDE_BIN=/abs/path/to/claude)", flush=True)
        return
    try:
        subprocess.run([_claude_bin, "auth", "status"],
                       stdin=subprocess.DEVNULL, capture_output=True,
                       text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        pass


# ── Usage API ─────────────────────────────────────────────────────────────────
# A usage reading is a dict: {session_pct, weekly_pct, session_reset, weekly_reset,
# rate_limited}. Percentages are 0-100 (or None); resets are seconds-until-reset
# (or None); rate_limited is a bool. The firmware ignores any fields that are None.

def _clamp_pct(value) -> int | None:
    if value is None or not isinstance(value, (int, float)):
        return None
    try:
        return round(max(0, min(100, value)))
    except (ValueError, TypeError):
        return None


def _reset_secs(window: dict) -> int | None:
    """Convert a window's `resets_at` (ISO-8601 string or epoch seconds) to seconds-from-now."""
    raw = window.get("resets_at") if isinstance(window, dict) else None
    if raw is None:
        return None
    try:
        if isinstance(raw, (int, float)):
            target = float(raw)
        else:
            # ISO-8601, e.g. "2026-05-31T15:00:00Z"
            target = datetime.fromisoformat(str(raw).replace("Z", "+00:00")).timestamp()
    except (ValueError, TypeError):
        return None
    return max(0, int(target - time.time()))


def fetch_usage(token: str) -> dict | None:
    """Read usage from the OAuth usage API. Returns a usage dict, or None on failure."""
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
                return None
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 401:
            print("[companion] usage API 401 — token expired, will re-read keychain", flush=True)
        else:
            print(f"[companion] usage API HTTP {e.code}", flush=True)
        return None
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as e:
        print(f"[companion] usage API error: {e}", flush=True)
        return None

    five = data.get("five_hour") or {}
    seven = data.get("seven_day") or {}
    session = _clamp_pct(five.get("utilization"))
    weekly  = _clamp_pct(seven.get("utilization"))
    if session is None and weekly is None:
        return None
    return {
        "session_pct":   session,
        "weekly_pct":    weekly,
        "session_reset": _reset_secs(five),
        "weekly_reset":  _reset_secs(seven),
        "rate_limited":  (session is not None and session >= 100)
                         or (weekly is not None and weekly >= 100),
    }


def fetch_usage_via_messages(token: str) -> dict | None:
    """Fallback (Clawdmeter-style): 1-token /v1/messages call, read ratelimit headers.

    Costs ~1 Haiku token. Header names are read defensively; on first use the full set
    of anthropic-ratelimit-* headers is logged so they can be confirmed.
    """
    body = json.dumps({
        "model": "claude-3-5-haiku-latest",
        "max_tokens": 1,
        "messages": [{"role": "user", "content": "."}],
    }).encode()
    req = urllib.request.Request(
        MESSAGES_API_URL,
        data=body,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-version": ANTHROPIC_VERSION,
            "anthropic-beta": USAGE_API_BETA,
            "User-Agent": USAGE_USER_AGENT,
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            headers = {k.lower(): v for k, v in resp.headers.items()}
            resp.read()
    except urllib.error.HTTPError as e:
        headers = {k.lower(): v for k, v in (e.headers or {}).items()}  # rate headers ride 429s too
        if not headers:
            print(f"[companion] messages fallback HTTP {e.code}", flush=True)
            return None
    except (urllib.error.URLError, TimeoutError) as e:
        print(f"[companion] messages fallback error: {e}", flush=True)
        return None

    rl = {k: v for k, v in headers.items() if k.startswith("anthropic-ratelimit")}
    if rl:
        print(f"[companion] ratelimit headers: {rl}", flush=True)  # one-time confirmation aid

    def _hdr_pct(*names):
        for n in names:
            if n in headers:
                try:
                    return _clamp_pct(float(headers[n]))
                except (ValueError, TypeError):
                    pass
        return None

    session = _hdr_pct("anthropic-ratelimit-unified-5h-utilization",
                       "anthropic-ratelimit-unified-utilization")
    weekly  = _hdr_pct("anthropic-ratelimit-unified-7d-utilization",
                       "anthropic-ratelimit-unified-weekly-utilization")
    status  = headers.get("anthropic-ratelimit-unified-status", "")
    if session is None and weekly is None:
        return None
    return {
        "session_pct":   session,
        "weekly_pct":    weekly,
        "session_reset": None,   # header reset times vary; omit unless confirmed
        "weekly_reset":  None,
        "rate_limited":  status not in ("", "allowed")
                         or (session is not None and session >= 100)
                         or (weekly is not None and weekly >= 100),
    }


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


def get_usage(_nudged: bool = False) -> dict | None:
    """Return a usage dict (see fetch_usage), or None. Primary: /api/oauth/usage;
    fallback: /v1/messages headers. Nudges the CLI to refresh a stale token once."""
    token = _tokens.get(force_refresh=True)
    if not token:
        if not _nudged:
            print("[companion] token stale — nudging `claude auth status` to refresh", flush=True)
            _nudge_cli_refresh()
            return get_usage(_nudged=True)
        print("[companion] no valid Claude Code OAuth token (is the claude CLI logged in?)",
              flush=True)
        return None
    usage = fetch_usage(token)
    if usage is None:
        if not _nudged:
            # Token may have just expired (401). Nudge a refresh and retry once.
            _nudge_cli_refresh()
            return get_usage(_nudged=True)
        # Primary failed even with a fresh token — try the messages-header fallback.
        print("[companion] primary usage API failed — trying /v1/messages fallback", flush=True)
        usage = fetch_usage_via_messages(token)
    return usage


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
                      f"pushing every {PUSH_INTERVAL // 60} min  (Ctrl-C to stop)", flush=True)
                while client.is_connected:
                    usage = get_usage()
                    # Send only the fields we actually have; firmware leaves the rest at -1.
                    payload_obj = {}
                    if usage:
                        for k in ("session_pct", "weekly_pct", "session_reset", "weekly_reset"):
                            if usage.get(k) is not None:
                                payload_obj[k] = usage[k]
                        if usage.get("rate_limited"):
                            payload_obj["rate_limited"] = True
                    if payload_obj:
                        payload = json.dumps(payload_obj) + "\n"
                        print(f"[companion] → {payload.strip()}", flush=True)
                        try:
                            await client.write_gatt_char(char_uuid, payload.encode(), response=True)
                            print("[companion] write ack OK", flush=True)
                        except Exception as we:
                            print(f"[companion] write failed: {we}", flush=True)
                            break
                    else:
                        print("[companion] no usage data this cycle (skipping push)", flush=True)
                    # Sleep in short slices so a dropped link is noticed within a few
                    # seconds (and reconnects) rather than after the full 5-min interval.
                    for _ in range(PUSH_INTERVAL // 5):
                        if not client.is_connected:
                            break
                        await asyncio.sleep(5)
        except Exception as exc:
            print(f"[companion] connection error ({exc}) — reconnecting in 5s", flush=True)
            await asyncio.sleep(5)


def main():
    if "--dry-run" in sys.argv:
        print("[companion] DRY RUN — querying real Anthropic usage API (no BLE)")
        usage = get_usage()
        if not usage:
            print("  Could not read usage. Is Claude Code logged in on this machine?")
            print("  (First run prompts for Keychain access — click Always Allow.)")
        else:
            def _fmt(secs):
                if secs is None:
                    return "?"
                m = secs // 60
                return f"{m // 1440}d" if m >= 1440 else f"{m // 60}h" if m >= 60 else f"{m}m"
            print(f"  session_pct (5h)  = {usage['session_pct']}%   resets in {_fmt(usage['session_reset'])}")
            print(f"  weekly_pct  (7d)  = {usage['weekly_pct']}%   resets in {_fmt(usage['weekly_reset'])}")
            print(f"  rate_limited      = {usage['rate_limited']}")
        return

    print("[companion] using the official Anthropic usage API for real percentages")
    print("[companion] if this hangs on first run, approve the Keychain prompt (Always Allow)")
    try:
        asyncio.run(push_loop())
    except KeyboardInterrupt:
        print("\n[companion] stopped")


if __name__ == "__main__":
    main()
