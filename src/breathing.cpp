#include "breathing.h"
#include "app_globals.h"
#include <M5StickCPlus.h>
#include "leds.h"
#include "character.h"
#include "buddy.h"
#include "buddy_clawd.h"

static bool     breathOpen    = false;
static uint32_t breathStartMs = 0;
static bool     breathDirty   = false;   // (set on entry / pattern change; drawBreath full-repaints)

// Offscreen buddy canvas for the breathing zoom (PSRAM, lazy-init in breathingDraw).
static TFT_eSprite breathBuddy = TFT_eSprite(&M5.Lcd);

bool     breathingActive()  { return breathOpen; }
uint32_t breathingStartMs() { return breathStartMs; }

void breathingEnter(uint32_t now) {
  breathOpen = true; breathStartMs = now; breathDirty = true;
  beep(1500, 60);
}

void breathingClose() { breathOpen = false; }

void breathingButtons(uint32_t now) {
  // Breathing mode owns the buttons: A or C exits, B cycles the pattern.
  if ((M5.BtnA.wasReleased() && !swallowBtnA) || (M5.BtnC.wasPressed() && !swallowBtnC)) {
    breathOpen = false;
    beep(700, 40);
    M5.Lcd.fillScreen(characterPalette().bg);
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    if (clawdMode) clawdInvalidate();
    applyDisplayMode();
  } else if (M5.BtnB.wasPressed() && !swallowBtnB) {
    ledsSetBreath((ledsBreathIdx() + 1) % BREATH_COUNT);
    breathStartMs = now;        // restart the cycle on the new pattern
    breathDirty = true;
    beep(1500, 40);
  }
}

void breathingDraw(uint32_t now, bool connected, bool lowBattery) {
  const Palette& p = characterPalette();
  uint32_t bt = ledsBreathClock(now);
  BreathPhase ph; uint8_t secs; const char* label;
  uint8_t lvl = ledsBreathInfo(bt, &ph, &secs, &label);
  uint32_t cyc = ledsBreathCycleMs();
  uint16_t cycleNum = cyc ? (uint16_t)(bt / cyc) + 1 : 1;

  // Lazy-init the offscreen buddy canvas in PSRAM. 320 wide so the buddy's
  // fixed x-center (160) lands in it; pivot at the buddy's center for the zoom.
  if (!breathBuddy.getBuffer()) {
    breathBuddy.setColorDepth(16);
    breathBuddy.setPsram(true);
    breathBuddy.createSprite(320, 110);
    breathBuddy.setPivot(W / 2, 42);
  }

  spr.fillSprite(p.bg);

  // Pattern name (top-left) + cycle counter (top-right).
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(ledsBreathName(), 6, 6);
  spr.setTextDatum(TR_DATUM);
  char cbuf[16]; snprintf(cbuf, sizeof(cbuf), "cycle %u", cycleNum);
  spr.drawString(cbuf, W - 6, 6);

  // The buddy, scaled by the breath (calm pose). Black is transparent.
  breathBuddy.fillScreen(0x0000);
  if (clawdMode) {
    clawdSetContext(connected, lowBattery, true);  // breathing scene → calm Clawd
    clawdRenderTo(&breathBuddy, P_IDLE);
  } else {
    buddyRenderTo(&breathBuddy, P_SLEEP);
  }
  float zoom = 1.0f + (lvl / 255.0f) * 1.3f;          // 1.0 (empty) .. 2.3 (full)
  breathBuddy.pushRotateZoom(&spr, W / 2, 116, 0.0f, zoom, zoom, 0x0000);

  // Phase word + per-phase countdown. Cyan-blue on the inhale, indigo/violet on
  // the exhale so the two halves of the cycle read apart at a glance — the
  // buddy's zoom alone looks the same mid-inhale and mid-exhale. Matches the bar.
  uint16_t phaseColor;
  switch (ph) {
    case BR_INHALE: phaseColor = spr.color565(90, 190, 255); break;  // cyan-blue
    case BR_EXHALE: phaseColor = spr.color565(150, 90, 255); break;  // indigo/violet
    default:        phaseColor = p.text;                     break;  // holds: neutral
  }
  spr.setTextDatum(TC_DATUM);
  spr.setTextSize(3);
  spr.setTextColor(phaseColor, p.bg);
  char line[24]; snprintf(line, sizeof(line), "%s %u", label, secs);
  spr.drawString(line, W / 2, 206);

  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}
