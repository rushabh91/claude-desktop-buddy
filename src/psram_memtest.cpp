// ─────────────────────────── PSRAM diagnostic ───────────────────────────────
// Standalone build: pio run -e psram-memtest -t upload
//
// The Fire's 4MB PSRAM is detected but corrupts the heap under real use with
// BLE active (see docs/PSRAM-DEBUG.md). This isolates the cause:
//   Test 1 — full write/verify with BLE OFF   → is the chip electrically sound?
//   Test 2 — full write/verify with BLE ON    → does the BLE radio trigger it?
//   loop() — continuous PSRAM stress + BLE advertising → does it survive use?
//
// Results render on the LCD (we must NOT call Serial.begin() — it crashes this
// board; ets_printf is a safe ROM-console fallback). This build deliberately
// does NOT install the internal-RAM guard, so allocations behave normally.

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "ble_bridge.h"

extern "C" int ets_printf(const char*, ...);

static M5Canvas g_canvas(&M5.Display);   // full-screen sprite, allocated in PSRAM
static bool     g_made = false;

static int _row = 0;
static void say(const char* s, uint16_t color = 0xFFFF) {
  M5.Display.setTextColor(color, 0x0000);
  M5.Display.setCursor(6, 6 + _row * 18);
  M5.Display.print(s);
  _row++;
  ets_printf("MEMTEST: %s\n", s);
}

// Full write/verify of an N-byte PSRAM block across several patterns.
// Returns true if every byte read back correctly; reports the failure address.
static bool psramTest(size_t N) {
  uint8_t* p = (uint8_t*)heap_caps_malloc(N, MALLOC_CAP_SPIRAM);
  if (!p) { say("  alloc FAIL", 0xF800); return false; }

  char buf[48];
  const uint8_t pats[] = { 0x00, 0xFF, 0xAA, 0x55 };
  for (uint8_t pat : pats) {
    memset(p, pat, N);
    for (size_t i = 0; i < N; i++) {
      if (p[i] != pat) {
        snprintf(buf, sizeof(buf), "  fail pat %02X @ %u", pat, (unsigned)i);
        say(buf, 0xF800);
        heap_caps_free(p);
        return false;
      }
    }
  }
  // address-as-data (catches stuck address lines / aliasing)
  for (size_t i = 0; i < N; i++) p[i] = (uint8_t)(i * 31 + 7);
  for (size_t i = 0; i < N; i++) {
    if (p[i] != (uint8_t)(i * 31 + 7)) {
      snprintf(buf, sizeof(buf), "  fail addr @ %u", (unsigned)i);
      say(buf, 0xF800);
      heap_caps_free(p);
      return false;
    }
  }
  heap_caps_free(p);
  return true;
}

void setup() {
  M5.begin();
  M5.Display.setRotation(1);
  M5.Display.fillScreen(0x0000);
  M5.Display.setTextSize(2);

  char buf[48];
  size_t psFree    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t psLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  size_t inFree    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  snprintf(buf, sizeof(buf), "PSRAM %uKB free", (unsigned)(psFree / 1024));    say(buf, 0x07FF);
  snprintf(buf, sizeof(buf), "  block %uKB",    (unsigned)(psLargest / 1024)); say(buf, 0x07FF);
  snprintf(buf, sizeof(buf), "IRAM  %uKB free", (unsigned)(inFree / 1024));    say(buf, 0x07FF);

  // Test 1: chip health with BLE off (2 MB exercised).
  say("Test1 BLE off (2MB)");
  bool ok1 = psramTest(2 * 1024 * 1024);
  say(ok1 ? "Test1 PASS" : "Test1 FAIL", ok1 ? 0x07E0 : 0xF800);

  // Bring BLE up exactly as the real firmware does, then re-test.
  say("init BLE...");
  bleInit("memtest-psram");
  say("Test2 BLE on (2MB)");
  bool ok2 = psramTest(2 * 1024 * 1024);
  say(ok2 ? "Test2 PASS" : "Test2 FAIL", ok2 ? 0x07E0 : 0xF800);

  // Test 3: the real scenario — a full-screen 320x240 sprite in PSRAM, which
  // frees ~65KB of internal RAM. loop() then animates + pushes it every frame
  // with BLE active (DMA/cache reads from PSRAM during SPI flush — the thing
  // the raw test never exercised and what Phase 0 boot-looped on).
  size_t inBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t psBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  g_canvas.setPsram(true);
  g_canvas.createSprite(320, 240);
  g_made = (g_canvas.getBuffer() != nullptr);
  size_t inAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t psAfter = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  char buf2[48];
  snprintf(buf2, sizeof(buf2), "Test3 sprite %s", g_made ? "OK" : "NULL");
  say(buf2, g_made ? 0x07E0 : 0xF800);
  // If it landed in PSRAM, PSRAM free drops ~150KB and internal barely moves.
  snprintf(buf2, sizeof(buf2), " ps-%uKB in-%uKB",
           (unsigned)((psBefore - psAfter) / 1024),
           (unsigned)((inBefore > inAfter ? inBefore - inAfter : 0) / 1024));
  say(buf2, 0x07FF);

  say("loop: push+BLE...", 0xFFE0);
  delay(6000);   // hold the results on screen before loop() takes over the LCD
}

void loop() {
  M5.update();

  // The decisive test: animate into the PSRAM sprite and flush it to the LCD
  // every frame with BLE active. This is exactly what the real firmware does
  // and what Phase 0 boot-looped on. Smooth disc + climbing frame counter with
  // no reset → a full-screen PSRAM sprite is viable.
  static uint32_t frame = 0;
  if (g_made) {
    g_canvas.fillScreen(0x0000);
    int x = 40 + (int)(frame % 240);
    g_canvas.fillCircle(x, 150, 40, 0x051F);
    g_canvas.setTextColor(0x07E0, 0x0000);
    g_canvas.setTextSize(2);
    char b[40];
    snprintf(b, sizeof(b), "PSRAM sprite push %lu", (unsigned long)frame);
    g_canvas.drawString(b, 6, 6);
    g_canvas.drawString("BLE active", 6, 30);
    g_canvas.pushSprite(0, 0);
    frame++;
  } else {
    M5.Display.fillRect(0, 232, 320, 8, 0x0000);
    M5.Display.setCursor(6, 232);
    M5.Display.setTextColor(0xF800, 0x0000);
    M5.Display.print("sprite NULL - cannot push");
    delay(500);
  }
  delay(10);
}
