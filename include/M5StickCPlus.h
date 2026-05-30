// ---------------------------------------------------------------------------
// Compatibility shim: M5StickCPlus / TFT_eSPI API  ->  M5Unified + M5GFX
// ---------------------------------------------------------------------------
// The example firmware was written for the M5StickC Plus. This header lets the
// same source build for the M5Stack Fire (and other M5Unified-supported
// boards) with only a handful of edits in main.cpp / data.h / xfer.h.
//
// What's different on the Fire, and how it's handled here:
//   * Power: IP5306, not AXP192 -> the AXP shim maps brightness/power-off onto
//     M5.Display / M5.Power and stubs the readings the IP5306 can't provide
//     (charge current, VBUS voltage, chip temperature, AXP power button).
//   * Buzzer: I2S speaker -> the Beep shim forwards to M5.Speaker.
//   * RTC: none on the Fire -> the Rtc shim just round-trips values. The
//     USB-only clock face is effectively disabled because GetVBusVoltage()
//     returns 0, so `_onUsb` never becomes true.
//   * Display: 320x240 landscape vs 135x240 portrait. The UI still renders at
//     its original 135x240 size in the top-left of the panel (a follow-up,
//     not part of this minimal port).
//
// The BLE protocol, character/buddy rendering and approve/deny logic are
// board-agnostic and are reused unchanged.
#pragma once

#include <M5Unified.h>

// M5GFX already hoists its color constants (GREEN, RED, ...) to global scope.
// Pull in ONLY the text datums and the LovyanGFX base class that this code
// references — a blanket `using namespace m5gfx;` would also drag lgfx::delay()
// into global scope and make Arduino's delay() calls ambiguous.
using lgfx::v1::TL_DATUM;
using lgfx::v1::MC_DATUM;

// Old TFT_eSPI type names -> LovyanGFX equivalents:
//   TFT_eSPI    : polymorphic draw surface (display OR sprite), used only as a
//                 base-class pointer (TFT_eSPI*) by the render helpers.
//   TFT_eSprite : the off-screen canvas (the global `spr`).
using TFT_eSPI    = lgfx::LovyanGFX;
using TFT_eSprite = M5Canvas;

// ---- AXP192 power management shim (Fire uses IP5306 via M5.Power) ----------
struct _AxpCompat {
  // Stick code passes a 0..100 brightness; map to the 0..255 backlight range.
  void ScreenBreath(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    M5.Display.setBrightness((uint8_t)(pct * 255 / 100));
  }
  // LDO2 powered the stick's backlight. "Off" -> backlight to 0; "on" is a
  // no-op because callers follow it with applyBrightness().
  void SetLDO2(bool on) { if (!on) M5.Display.setBrightness(0); }
  float   GetVBusVoltage()    { return 0.0f; }   // no VBUS read on IP5306 (keeps the clock face off)
  float   GetBatVoltage()     { return M5.Power.getBatteryVoltage() / 1000.0f; }
  float   GetBatCurrent()     { return 0.0f; }   // IP5306 can't report charge current
  float   GetTempInAXP192()   { return 0.0f; }   // no AXP temperature sensor
  uint8_t GetBtnPress()       { return 0; }      // no AXP power button on the Fire
  void    PowerOff()          { M5.Power.powerOff(); }
};
extern _AxpCompat Axp;

// ---- Buzzer shim (Fire has an I2S speaker via M5.Speaker) ------------------
struct _BeepCompat {
  void begin()                       { M5.Speaker.begin(); }
  void tone(uint16_t f, uint16_t ms) { M5.Speaker.tone((float)f, (uint32_t)ms); }
  void update()                      {}   // M5.Speaker is non-blocking; M5.update() services it
};
extern _BeepCompat Beep;

// ---- RTC shim (Fire has no on-board RTC) -----------------------------------
// Only needs to satisfy the API and round-trip values; the clock face that
// consumes it is disabled on the Fire (see GetVBusVoltage above).
struct RTC_TimeTypeDef { uint8_t Hours, Minutes, Seconds; };
struct RTC_DateTypeDef { uint8_t WeekDay, Month, Date; uint16_t Year; };
struct _RtcCompat {
  RTC_TimeTypeDef _t {};
  RTC_DateTypeDef _d {};
  void SetTime(const RTC_TimeTypeDef* t) { _t = *t; }
  void SetDate(const RTC_DateTypeDef* d) { _d = *d; }
  void GetTime(RTC_TimeTypeDef* t)       { *t = _t; }
  void GetDate(RTC_DateTypeDef* d)       { *d = _d; }
};
// Defined once in main.cpp (extern here) — avoids C++17 inline variables so
// the project builds under the default gnu++11 standard.
extern _RtcCompat Rtc;
