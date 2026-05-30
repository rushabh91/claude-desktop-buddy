# PSRAM boot-crash dossier (M5Stack Fire port)

Handoff notes for getting the Fire's **4 MB PSRAM** working. The current
firmware on `main` **deliberately avoids PSRAM** (everything in internal RAM) to
boot reliably. This documents the crashes, root-cause analysis, what was tried,
and concrete next steps.

---

## TL;DR

- The Fire's 4 MB external PSRAM is **detected, sized (≈4,128,756 B), passes its
  boot self-test, and is added to the heap** — but **corrupts under sustained
  real use**. Any allocation that lands in PSRAM (the SDK routes allocations
  `>4 KB` there) eventually trashes the heap, surfacing as FreeRTOS lock asserts
  or heap-walk faults at semi-random points (layout-dependent → classic memory
  corruption).
- It is **intermittent**, not dead: in the PSRAM-sprite build the buddy *did*
  render (sleeping) on screen before crashing — so the chip responds; this looks
  like **marginal timing**, not a fully dead chip.
- **Current workaround (on `main`):** force all allocations to internal RAM via
  `heap_caps_malloc_extmem_enable(0xFFFFFFFFU)` + `spr.setPsram(false)`. PSRAM is
  registered but never written. Device is stable.

## Hardware & toolchain (exact)

| Thing | Value |
|---|---|
| Board | `m5stack-fire` (PlatformIO) |
| MCU | **ESP32-D0WDQ6-V3, silicon revision v3.0** (dual-core, 240 MHz, 40 MHz crystal) |
| MAC | `78:21:84:AA:B2:1C` |
| PSRAM | 4 MB **external** chip (not embedded; D0WDQ6 has none on-die) |
| Flash | 16 MB, `flash_mode = dio`, `f_flash = 40 MHz` (board default) |
| Platform | `espressif32 @ 7.0.1` |
| Framework | arduino-esp32 **2.0.17** (`framework-arduinoespressif32 @ 3.20017`) → **Bluedroid 2.x BLE API** |
| Libs | `M5Unified 0.2.16`, `M5GFX 0.2.22`, `AnimatedGIF 2.2.2`, `ArduinoJson 7.4.3` |
| Board `extra_flags` | `-DARDUINO_M5STACK_FIRE -DBOARD_HAS_PSRAM -mfix-esp32-psram-cache-issue -mfix-esp32-psram-cache-strategy=memw` |

**Key mismatch:** `-mfix-esp32-psram-cache-issue` is the **ESP32 rev1** PSRAM
cache-bug workaround. This chip is **rev3** (bug fixed in silicon), so the
workaround is unnecessary and may interact badly. The precompiled SDK also has
`CONFIG_SPIRAM_CACHE_WORKAROUND=y` baked in.

## Relevant SDK config (`framework-arduinoespressif32/tools/sdk/esp32/sdkconfig`)

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_SIZE=-1
CONFIG_SPIRAM_SPEED_80M=y                  # PSRAM @ 80MHz (flash defaults to 40MHz!)
# CONFIG_SPIRAM_BOOT_INIT is not set       # PSRAM init done by Arduino core, not bootloader
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096   # allocations >4KB go to PSRAM
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=0
CONFIG_SPIRAM_CACHE_WORKAROUND=y
CONFIG_SPIRAM_CACHE_WORKAROUND_STRATEGY_MEMW=y
```

- PSRAM init path: `initArduino()` → `psramInit()` (`cores/esp32/esp32-hal-psram.c`),
  gated by `#if CONFIG_SPIRAM_SUPPORT || CONFIG_SPIRAM` — **NOT** by
  `-DBOARD_HAS_PSRAM`. So **you cannot disable PSRAM by removing the define**;
  the core always runs `esp_spiram_init → init_cache → testSPIRAM →
  esp_spiram_add_to_heapalloc → heap_caps_malloc_extmem_enable(4096)`.
- `esp_get_free_heap_size()` ≈ 90 KB (internal) after BLE init;
  `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` ≈ 4 MB (PSRAM) early in boot.

## The crashes (decoded backtraces)

Decode with:
`~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/m5stack-fire/firmware.elf <addr>...`

### Crash A — PSRAM allocation (the original, headline PSRAM crash)
```
assert failed: spinlock_acquire spinlock.h:122 (result == core_id || result == SPINLOCK_FREE)
```
Call chain (top app frames):
```
setup() main.cpp  -> spr.createSprite(135,240)
  LGFX_Sprite::createSprite (LGFX_Sprite.hpp:174)
   Panel_Sprite::createSprite (LGFX_Sprite.cpp:73)
    SpriteBuffer::reset(len, AllocationSource::Psram) (SpriteBuffer.cpp:131)
     heap_alloc_psram -> heap_caps_malloc(len, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)
      multi_heap_malloc -> multi_heap_internal_lock -> vPortEnterCritical
       spinlock_acquire (spinlock.h:122)  <-- ASSERT
```
`M5Canvas` defaults `_psram = true`, so the 64 KB sprite is the **first PSRAM
allocation** and it faults inside the PSRAM heap's own spinlock.

### Crash C — walking the PSRAM heap free-list
```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited). EXCVADDR: 0x83c44458  A8: 0x44444444
```
Call chain:
```
setup() main.cpp -> heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)   # our diagnostic
  heap_caps_get_info -> multi_heap_get_info -> tlsf_walk_pool (heap_tlsf.c:642)
   block_size/block_is_last  <-- reads a garbage block header (0x83c44458) -> LoadProhibited
```
**Decisive:** earlier in the same boot the *identical* call returned ≈4 MB and
walked fine; by this point walking the 8-bit heap (which includes the PSRAM
pool) reads **garbage TLSF metadata** → the **PSRAM region's heap metadata got
corrupted** between the two points, even though our code never explicitly wrote
PSRAM. `A8 = 0x44444444` is a junk pointer pattern.

### Crash B — UART driver (SEPARATE issue, not PSRAM; documented so you don't re-trip it)
```
assert failed: xQueueSemaphoreTake queue.c:1549 (pxQueue->uxItemSize == 0)
... Serial.println -> HardwareSerial::write -> uart_write_bytes -> uart_tx_all
     -> xQueueSemaphoreTake  (UART0 driver tx_mux invalid)
```
Calling **`Serial.begin()`** (installing the UART0 driver) crashed boot on this
board. **Fix applied:** don't call `Serial.begin()` — Serial.* debug calls
become safe no-ops, BLE is the transport. If you re-enable Serial while chasing
PSRAM, you'll hit this; root cause of the UART crash itself is unconfirmed (may
be entangled with the same memory corruption, or a UART0/console conflict).

## What was tried (and the result)

| Attempt | Result |
|---|---|
| `spr.setPsram(false)` (sprite → internal) | Fixed Crash A; exposed later corruption elsewhere |
| `heap_caps_malloc_extmem_enable(0xFFFFFFFFU)` at `setup()` start (force ALL allocs internal) | **Works** — PSRAM never written → stable. This is the current workaround. |
| `board_build.f_flash = 80000000L` (match flash to PSRAM's 80 MHz) | **No fix** — crash persisted identically (boot showed `clock div:1`, so 80 MHz did take effect) |
| Remove `-DBOARD_HAS_PSRAM` | Wouldn't disable PSRAM (init gated by `CONFIG_SPIRAM`, precompiled) — not pursued |
| `-std=gnu++17` vs `gnu++11` | Ruled out — not the cause |
| Remove `Serial.begin()` | Fixed Crash B (separate UART issue) |

## Current PSRAM-avoidance code (what to UNDO when re-enabling PSRAM)

In `src/main.cpp` `setup()`:
- `heap_caps_malloc_extmem_enable(0xFFFFFFFFU);` (~line 956) — remove to let allocations use PSRAM again.
- `spr.setPsram(false);` (~line 985) — remove / set true to put the sprite in PSRAM.

(Unrelated: `M5.Lcd.setRotation(1)` ~line 966 = display orientation; not PSRAM.)

## Recommended plan to get PSRAM working

**Do these in order — step 1 is essential before anything else.**

1. **Isolation memtest first.** Flash a *minimal* sketch (no BLE, no M5GFX —
   just `M5.begin()` or even bare Arduino) that does:
   `psramInit(); Serial-or-ets_printf psramFound(), ESP.getPsramSize(), ESP.getFreePsram();`
   then a **full 4 MB write→read→verify** of several patterns
   (`0x00/0xFF/0xAA/0x55/address-as-data`). This tells you definitively whether
   the PSRAM is **electrically reliable at all** on this unit, isolating
   "marginal chip/timing" from "our firmware/SDK config." Use `ets_printf` for
   output (works without `Serial.begin`, which crashes — see Crash B).

2. **Address the rev3 / cache-workaround mismatch.** This chip is **rev3**; the
   `-mfix-esp32-psram-cache-issue` workaround is for rev1. The workaround is also
   compiled into the precompiled SDK (`CONFIG_SPIRAM_CACHE_WORKAROUND=y`), so you
   **cannot fully remove it with build flags** on arduino-esp32 2.0.17. Options:
   - **Try the `pioarduino` platform / arduino-esp32 3.x (ESP-IDF 5.x).** Newer
     PSRAM init + per-chip-rev handling; the Fire's rev3 PSRAM is more likely to
     work cleanly. **Cost:** arduino-esp32 3.x changed the Bluedroid BLE API —
     `src/ble_bridge.cpp` uses 2.x (`BLESecurityCallbacks`,
     `getValue()→std::string`, `server->getConnId()`); you'd port it to the 3.x
     API or to **NimBLE-Arduino** (likely the cleaner choice; smaller, and the
     NUS service maps over directly).
   - **Or rebuild arduino-esp32 from source** (`esp32-arduino-lib-builder`) with
     `CONFIG_SPIRAM_SPEED_40M` and/or the cache workaround disabled — definitive
     but heavy.

3. **PSRAM speed.** `CONFIG_SPIRAM_SPEED_80M` is fixed in the precompiled lib.
   Matching flash to 80 MHz did **not** help. Dropping PSRAM to 40 MHz needs a
   custom SDK build (step 2's second option) — worth trying since 80 MHz external
   PSRAM is the most timing-sensitive config.

4. **If the memtest in step 1 fails even in isolation**, it's a marginal/faulty
   PSRAM chip on this specific unit → PSRAM may simply not be reliably usable
   here; keep the internal-RAM workaround.

## Build / flash / observe (repro)

```bash
cd ~/claude-desktop-buddy
export PATH="/opt/homebrew/bin:$PATH"
pio run -e m5stack-fire                                              # build
pio run -e m5stack-fire -t upload --upload-port /dev/cu.usbserial-54BB0239121   # flash
```

**Observe boot WITHOUT relying on the Arduino UART driver** (we don't call
`Serial.begin`). Read the port passively; a boot loop shows as repeated ROM
`rst:...SPI_FAST_FLASH_BOOT` lines:

```python
import serial, time
p = serial.Serial(); p.port='/dev/cu.usbserial-54BB0239121'; p.baudrate=115200
p.timeout=0.2; p.dtr=False; p.rts=False; p.open()   # do NOT toggle DTR/RTS
end=time.time()+12
buf=b''
while time.time()<end: buf+=p.read(512)
print(buf.decode('utf-8','replace'))
```

**Gotchas learned the hard way:**
- **Verify behavior on the SCREEN, not via serial captures.** Toggling DTR/RTS
  to force a reset for logging can *induce/misrepresent* crashes vs a normal
  power-on boot. The device rendered the buddy fine when serial logs (via reset)
  looked like a boot loop.
- `ets_printf("...")` (declare `extern "C" int ets_printf(const char*, ...);`)
  is a reliable ROM-console log that works without `Serial.begin`.
- Chip revision: `python3 ~/.platformio/packages/tool-esptoolpy/esptool.py
  --port /dev/cu.usbserial-54BB0239121 chip_id`
