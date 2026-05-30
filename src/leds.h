#pragma once
#include <FastLED.h>

// ───────────────────────── M5Stack Fire RGB LED bar ─────────────────────────
// The Fire has 10 SK6812 (GRB) addressable LEDs on GPIO15. They are NOT
// initialized by M5Unified, so at boot the floating data line clocks random
// bits into them and they glow white. ledsInit() drives the bar black at the
// earliest safe point to kill that glow, then it mirrors the device's
// PersonaState as an ambient status light.
//
// Buffer is a 30-byte file-static array (no heap, internal RAM). FastLED's RMT
// driver allocates only a tiny internal buffer, well clear of the PSRAM ban.

#define LEDS_PIN   15
#define LEDS_COUNT 10

// Mirrors PersonaState in main.cpp (same order). Passed as a plain int.
enum { LP_SLEEP, LP_IDLE, LP_BUSY, LP_ATTENTION, LP_CELEBRATE, LP_DIZZY, LP_HEART };

static CRGB     _leds[LEDS_COUNT];
static uint8_t  _ledPersona     = LP_SLEEP;
static bool     _ledEnabled     = true;
static uint8_t  _ledBright      = 96;          // global cap (battery-safe)
static uint32_t _ledFlashUntil  = 0;
static CRGB     _ledFlashColor   = CRGB::Black;
static bool     _ledLowBatt      = false;
static uint32_t _ledLastShow     = 0;

// ───────────────────────── Breathing-exercise pacer ─────────────────────────
// When idle, the bar guides paced breathing: brightness tracks "lung fullness"
// — ramp up to inhale, hold full, ramp down to exhale, hold dark. The dark
// hold gives a long restful pause. Phase durations are in ms.
struct BreathPattern { uint16_t inhale, hold1, exhale, hold2; const char* name; };
static const BreathPattern BREATHS[] = {
  { 4000, 4000, 4000, 4000, "box 4-4-4-4" },   // box breathing (default)
  { 4000, 7000, 8000,    0, "relax 4-7-8" },   // 4-7-8 relaxing breath
  { 5000,    0, 5000,    0, "coherent 5-5" },  // resonant/coherent breathing
};
static const uint8_t BREATH_COUNT = sizeof(BREATHS) / sizeof(BREATHS[0]);
static uint8_t _breathIdx = 0;

enum BreathPhase { BR_INHALE, BR_HOLD_FULL, BR_EXHALE, BR_HOLD_EMPTY };

inline void    ledsSetBreath(uint8_t idx) { _breathIdx = idx % BREATH_COUNT; }
inline uint8_t ledsBreathIdx()            { return _breathIdx; }
inline const char* ledsBreathName()       { return BREATHS[_breathIdx].name; }

// Brightness 0..255 across the current breath cycle (eased for a natural feel)
// and which phase we're in (for phase-colored LEDs / on-screen cues).
static uint8_t _breathLevel(uint32_t now, BreathPhase* phase) {
  const BreathPattern& b = BREATHS[_breathIdx];
  uint32_t cycle = (uint32_t)b.inhale + b.hold1 + b.exhale + b.hold2;
  if (!cycle) { *phase = BR_HOLD_EMPTY; return 0; }
  uint32_t t = now % cycle;
  if (t < b.inhale)  { *phase = BR_INHALE;    return ease8InOutCubic((uint8_t)((t * 255) / b.inhale)); }
  t -= b.inhale;
  if (t < b.hold1)   { *phase = BR_HOLD_FULL; return 255; }
  t -= b.hold1;
  if (t < b.exhale)  { *phase = BR_EXHALE;    return 255 - ease8InOutCubic((uint8_t)((t * 255) / b.exhale)); }
  *phase = BR_HOLD_EMPTY; return 0;
}

// Phase → bar color: blue while moving, white at the full hold, off when empty.
static void _renderBreath(uint32_t bt) {
  BreathPhase ph; uint8_t b = _breathLevel(bt, &ph);
  CRGB c;
  switch (ph) {
    case BR_HOLD_FULL:  c = CRGB(b, b, b);      break;
    case BR_HOLD_EMPTY: c = CRGB::Black;        break;
    default:            c = CRGB(0, b / 3, b);  break;
  }
  fill_solid(_leds, LEDS_COUNT, c);
}

// Dedicated breathing mode: when forced on, the bar breathes regardless of
// persona, on a clock that starts at `originMs` so it aligns with the on-
// screen guide (both call ledsBreathClock()).
static bool     _breathForce  = false;
static uint32_t _breathOrigin = 0;
inline void     ledsForceBreath(bool on, uint32_t originMs) { _breathForce = on; _breathOrigin = originMs; }
inline uint32_t ledsBreathClock(uint32_t now) { return _breathForce ? (now - _breathOrigin) : now; }
inline uint32_t ledsBreathCycleMs() {
  const BreathPattern& b = BREATHS[_breathIdx];
  return (uint32_t)b.inhale + b.hold1 + b.exhale + b.hold2;
}

// One call for the on-screen guide: returns level 0..255, and fills phase,
// whole seconds left in this phase, and the phase label ("Inhale"/...).
inline uint8_t ledsBreathInfo(uint32_t t, BreathPhase* phase, uint8_t* secsLeft, const char** label) {
  const BreathPattern& b = BREATHS[_breathIdx];
  uint32_t cycle = ledsBreathCycleMs();
  uint8_t lvl = _breathLevel(t, phase);
  uint32_t tt = cycle ? (t % cycle) : 0;
  uint32_t end;
  switch (*phase) {
    case BR_INHALE:    end = b.inhale; break;
    case BR_HOLD_FULL: end = (uint32_t)b.inhale + b.hold1; break;
    case BR_EXHALE:    end = (uint32_t)b.inhale + b.hold1 + b.exhale; break;
    default:           end = cycle; break;
  }
  uint32_t rem = (end > tt) ? (end - tt) : 0;
  *secsLeft = (uint8_t)((rem + 999) / 1000);
  static const char* L[] = { "Inhale", "Hold", "Exhale", "Rest" };
  *label = L[*phase];
  return lvl;
}

inline void ledsInit() {
  FastLED.addLeds<SK6812, LEDS_PIN, GRB>(_leds, LEDS_COUNT);
  FastLED.setBrightness(_ledBright);
  fill_solid(_leds, LEDS_COUNT, CRGB::Black);
  FastLED.show();                                // kill the white floating-line glow
}

// enabled = settings().led && !screenOff && !napping && !dnd (caller composes it).
// brightLevel 0..4 from the screen-brightness setting → battery-safe LED cap.
inline void ledsSetState(uint8_t persona, bool enabled, uint8_t brightLevel) {
  _ledPersona = persona;
  _ledEnabled = enabled;
  _ledBright  = 30 + brightLevel * 24;           // 30,54,78,102,126 of 255
}

inline void ledsFlash(const CRGB& c) { _ledFlashColor = c; _ledFlashUntil = millis() + 250; }
inline void ledsFlashApprove() { ledsFlash(CRGB::Green); }
inline void ledsFlashDeny()    { ledsFlash(CRGB::Red); }
inline void ledsLowBattery(bool on) { _ledLowBatt = on; }

inline void ledsTick(uint32_t now) {
  if (now - _ledLastShow < 33) return;           // ~30fps cap; never starve the loop
  _ledLastShow = now;
  FastLED.setBrightness(_ledBright);

  if (!_ledEnabled) {
    fill_solid(_leds, LEDS_COUNT, CRGB::Black);
    FastLED.show();
    return;
  }

  // One-shot event flash (approve/deny) overrides everything.
  if ((int32_t)(_ledFlashUntil - now) > 0) {
    fill_solid(_leds, LEDS_COUNT, _ledFlashColor);
    FastLED.show();
    return;
  }

  // Dedicated breathing mode: breathe regardless of persona, synced to the
  // on-screen guide via the shared breath clock.
  if (_breathForce) {
    _renderBreath(ledsBreathClock(now));
    FastLED.show();
    return;
  }

  switch (_ledPersona) {
    case LP_IDLE:                                 // ambient breathing pacer
      _renderBreath(now);
      break;
    case LP_BUSY: {                               // blue comet with fading tail
      fadeToBlackBy(_leds, LEDS_COUNT, 60);
      _leds[(now / 80) % LEDS_COUNT] = CRGB::Blue;
      break;
    }
    case LP_ATTENTION: {                          // amber 2Hz pulse — pending approval
      bool hi = (now / 250) % 2;
      fill_solid(_leds, LEDS_COUNT, hi ? CRGB(255, 120, 0) : CRGB(40, 18, 0));
      break;
    }
    case LP_CELEBRATE:                            // rainbow cycle
      fill_rainbow(_leds, LEDS_COUNT, (uint8_t)(now / 8), 25);
      break;
    case LP_DIZZY: {                              // sparkle
      fadeToBlackBy(_leds, LEDS_COUNT, 80);
      _leds[(now / 40) % LEDS_COUNT] = CHSV((uint8_t)now, 160, 255);
      break;
    }
    case LP_HEART: {                              // pink heartbeat double-pulse
      uint16_t ph = now % 1000;
      uint8_t b = (ph < 120 || (ph > 200 && ph < 320)) ? 255 : 30;
      fill_solid(_leds, LEDS_COUNT, CRGB(b, b / 5, (b * 2) / 5));
      break;
    }
    case LP_SLEEP:
    default:
      fill_solid(_leds, LEDS_COUNT, CRGB::Black);
      break;
  }

  // Low-battery overlay: slow red double-blink on LED 0, over any persona.
  if (_ledLowBatt) {
    uint16_t ph = now % 1600;
    if ((ph < 120) || (ph > 240 && ph < 360)) _leds[0] = CRGB::Red;
  }

  FastLED.show();
}
