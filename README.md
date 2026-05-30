# claude-desktop-buddy

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions. We've
been impressed by the creativity of the maker community around Claude -
providing a lightweight, opt-in API is our way of making it easier to build
fun little hardware devices that integrate with Claude.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

As an example, we built a desk pet on ESP32 that lives off permission
approvals and interaction with Claude. It sleeps when nothing's happening,
wakes when sessions start, gets visibly impatient when an approval prompt is
waiting, and lets you approve or deny right from the device.

<p align="center">
  <img src="docs/device.jpg" alt="M5StickC Plus running the buddy firmware" width="500">
</p>

## Hardware

The firmware targets two boards:

| Board | PlatformIO env | Screen | Notes |
|---|---|---|---|
| **M5Stack Fire** | `m5stack-fire` | 320×240 landscape ILI9341 | 3 buttons, IP5306 battery, 16MB flash, 4MB PSRAM |
| M5StickC Plus | *(upstream)* | 135×240 portrait ST7789 | 2 buttons + accel |

The Fire port uses a full-screen PSRAM sprite (320×240 @ 16bpp) that needs the 16MB partition table in `partitions_fire.csv` — it won't fit on the stock 4MB layout.

### Fire hardware notes

- **Battery:** IP5306 only reports 0/25/50/75/100% (no voltage readout). `isCharging()` works.
- **Clock:** no RTC — time is synced from the desktop app over BLE on connect.
- **No accelerometer** on Fire — shake/nap features are disabled.
- **Buttons:** BtnA (front), BtnB (right), BtnC (right, lower).

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then for M5Stack Fire:

```bash
pio run -e m5stack-fire -t upload
```

If you're starting from a previously-flashed device, wipe it first:

```bash
pio run -e m5stack-fire -t erase && pio run -e m5stack-fire -t upload
```

Once running, you can also wipe everything from the device itself: **hold A
→ settings → reset → factory reset → tap twice**.

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then, open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list. macOS will prompt for Bluetooth permission on
first connect; grant it.

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy… menu item" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window with Connect button and folder drop target" width="420">
</p>

Once paired, the bridge auto-reconnects whenever both sides are awake.

If discovery isn't finding the stick:

- Make sure it's awake (any button press)
- Check the stick's settings menu → bluetooth is on

## Controls (M5Stack Fire — 3-button scheme)

### Normal / transcript mode

| Button | Action |
|--------|--------|
| **A** (front) | Next screen (transcript → approval → info → …) |
| **B** (right, upper) | Scroll transcript |
| **C** (right, lower) | Enter breathing mode |
| **Hold A** | Open menu |
| **Power** (left, short press) | Toggle screen off/on |
| **Power** (left, ~6s hold) | Hard power off |

### Approval prompt

| Button | Action |
|--------|--------|
| **A** | ✓ Approve once |
| **B** | Dismiss (skip this prompt) |
| **C** | ✗ Deny |

### Menu navigation

| Button | Action |
|--------|--------|
| **A** | ▼ Move down |
| **C** | ▲ Move up |
| **B** | ● Select / confirm |

Menu hints are shown on-screen as `A▼ C▲ B●`.

### Breathing mode (BtnC from normal screen)

BtnC enters a breathing guide. The buddy scales with the breath. A pattern
picker cycles through box (4-4-4-4), 4-7-8, and coherent (5-5) patterns.
BtnC again, or BtnA, returns to normal. The RGB LED bar pulses in sync.

The screen auto-powers-off after 30s of no interaction (kept on while an
approval prompt is up). Any button press wakes it.

## Status bar (Fire landscape layout)

The top 20px of the screen is a persistent status bar:

| Position | Content |
|----------|---------|
| Left | BT icon (dim=unpaired, white=paired, green=encrypted) · device name |
| Center | `S__% W__%` — Claude Code session / weekly token usage (from usage companion) |
| Right | Battery level + charging indicator |
| Far right | Clock (`HH:MM`, synced over BLE) · DND moon (when do-not-disturb is on) |

## RGB LED bar

The Fire has a 10-pixel SK6812 RGB bar. Behavior:

| State | Color/pattern |
|-------|---------------|
| Idle / busy | Off (bar is dark unless breathing mode is active) |
| Breathing mode | Pulses with the breath cycle — matches the buddy scale animation |
| Low battery (<15%) | Slow red pulse (warning) |
| DND active | Bar stays dark |
| Per-persona | Each ASCII persona has a signature color used during breathing |

## Do Not Disturb

Menu → **DND** (or the top-level DND option) toggles a mode that silences
all melodies, turns the LED bar off, dims the screen, and shows a moon
icon in the status bar. Approval prompts still show; only sounds and LEDs
are suppressed.

## ASCII pets

Eighteen pets, each with seven animations (sleep, idle, busy, attention,
celebrate, dizzy, heart). Menu → "next pet" cycles them with a counter.
Choice persists to NVS.

## GIF pets

If you want a custom GIF character instead of an ASCII buddy, drag a
character pack folder onto the drop target in the Hardware Buddy window. The
app streams it over BLE and the stick switches to GIF mode live. **Settings
→ delete char** reverts to ASCII mode.

A character pack is a folder with `manifest.json` and 96px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate: each
loop-end advances to the next GIF, useful for an idle activity carousel so
the home screen doesn't loop one clip forever.

GIFs are 96px wide (Fire landscape: character is centered in the ~220px buddy area).
Crop tight to the character — transparent margins waste screen and shrink
the sprite. `tools/prep_character.py` handles the resize: feed it source
GIFs at any sizes and it produces a 96px-wide set where the character is the
same scale in every state.

The whole folder must fit under 1.8MB —
`gifsicle --lossy=80 -O3 --colors 64` typically cuts 40–60%.

See `characters/bufo/` for a working example.

If you're iterating on a character and would rather skip the BLE round-trip,
`tools/flash_character.py characters/bufo` stages it into `data/` and runs
`pio run -t uploadfs` directly over USB.

## The seven states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, **LED blinks**       |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | you shook the stick         | spiral eyes, wobbling       |
| `heart`     | approved in under 5s        | floating hearts             |

## Clawd persona

The compiled-in `clawd` persona is an RLE-encoded sprite of the Clawd mascot
rather than an ASCII buddy. It uses the same 7-state machine (sleep/idle/busy/
attention/celebrate/dizzy/heart) and renders at full size in the buddy area.
Cycle to it via menu → next pet.

## Usage companion

A host-side Python script pushes Claude Code token-usage percentages to the
device over BLE so the status bar shows `S__% W__%` (session / weekly).
See [`tools/usage_companion/README.md`](tools/usage_companion/README.md).

## Project layout

```
src/
  main.cpp           — loop, state machine, UI screens (landscape 320×240)
  buddy.cpp          — ASCII species dispatch + render helpers
  buddies/           — one file per species, seven anim functions each
  ble_bridge.cpp     — Nordic UART service, 2-connection support
  character.cpp      — GIF decode + render
  data.h             — wire protocol, JSON parse (incl. session_pct/weekly_pct)
  xfer.h             — folder push receiver
  stats.h            — NVS-backed stats, settings, owner, species choice
  leds.h             — SK6812 LED bar (breathing, low-battery, DND)
characters/          — example GIF character packs
tools/
  usage_companion/   — BLE companion that pushes usage % to the device
  ble_scan.py        — quick BLE scan helper
partitions_fire.csv  — 16MB partition table for M5Stack Fire
```

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.
