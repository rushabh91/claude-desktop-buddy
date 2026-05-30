#!/usr/bin/env python3
"""Quick BLE scan to see whether the M5 buddy is advertising and discoverable.

Usage: .venv/bin/python tools/ble_scan.py [seconds]
Prints every advertiser (name / address / RSSI / service UUIDs) and flags any
device whose name starts with "Claude" (the buddy's advertised name).
"""
import asyncio
import sys
from bleak import BleakScanner

NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"


async def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
    print(f"scanning {secs:.0f}s ...", flush=True)
    devices = await BleakScanner.discover(timeout=secs, return_adv=True)
    claude = []
    for addr, (dev, adv) in sorted(devices.items(), key=lambda kv: -(kv[1][1].rssi or -999)):
        name = adv.local_name or (dev.name or "")
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        has_nus = NUS in uuids
        print(f"  {addr}  rssi={adv.rssi:>4}  name={name!r:<24} nus={has_nus} uuids={uuids}", flush=True)
        if name.lower().startswith("claude") or has_nus:
            claude.append((addr, name, adv.rssi, has_nus))
    print("----")
    if claude:
        print("MATCH (buddy candidate):")
        for addr, name, rssi, has_nus in claude:
            print(f"  {name!r}  {addr}  rssi={rssi}  nus_service={has_nus}")
    else:
        print(f"No Claude/NUS advertiser seen among {len(devices)} devices.")


asyncio.run(main())
