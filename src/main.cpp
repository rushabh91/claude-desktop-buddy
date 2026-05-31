#include <M5StickCPlus.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);
TFT_eSprite breathBuddy = TFT_eSprite(&M5.Lcd);   // offscreen buddy for the breathing zoom

// Single definitions of the M5StickCPlus->M5Unified compat shims (declared
// extern in include/M5StickCPlus.h).
_AxpCompat  Axp;
_BeepCompat Beep;
_RtcCompat  Rtc;

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
#include "leds.h"
#include "melody.h"
#include "buddy_clawd.h"
const int W = 320, H = 240;   // full landscape panel = the PSRAM sprite size
const int CX = W / 2;         // 160
const int CY_BASE = 120;

// Where the 135x240 portrait sprite lands on the Fire's larger panel. Computed
// in setup() once the rotation is set; centers the stick-sized UI.
static int spritePushX = 0, spritePushY = 0;

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 0;           // 0..4 → ScreenBreath 20..100 (boot dim)
bool    btnALong    = false;
// Care menu (hold-B from home): feed / play / close. Mirrors the hold-A menu.
bool    careMenuOpen = false;
uint8_t careSel      = 0;
bool    btnBLong     = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;

// Dedicated breathing-exercise mode (entered with BtnC from the home screen).
// Takes over the screen + LED bar, paces a chosen pattern, ignores screen
// sleep. BtnB cycles patterns; BtnA/BtnC exit.
bool     breathOpen    = false;
uint32_t breathStartMs = 0;
bool     breathDirty   = false;   // force a full repaint (on entry / pattern change)

// Mini-games (launched from the care menu). Self-contained screen + LED bar,
// like breathing mode. C = quit; B = catch / play-again. Two types:
//   GAME_CATCH — LED cursor sweeps, press B at the center target.
//   GAME_DANCE — shake the device on each audible beat (IMU rhythm).
enum { GAME_CATCH = 0, GAME_DANCE = 1 };
bool     gameOpen     = false;
uint8_t  gameType     = GAME_CATCH;
uint8_t  gamePhase    = 0;        // 0 = playing, 1 = game over
int8_t   gameCursor   = 0;
int8_t   gameDir      = 1;
uint16_t gameStreak   = 0;        // score (hits) — shared "best" across both games
uint16_t gameStepMs   = 180;      // catch: cursor step interval; shrinks as you score
uint32_t gameNextStep = 0;
bool     gameNewBest  = false;
const int8_t  GAME_TGT_LO = 4, GAME_TGT_HI = 5;
const uint16_t GAME_STEP_START = 180, GAME_STEP_MIN = 60, GAME_STEP_DEC = 12;
// Shake Dance state.
uint16_t danceCombo   = 0;
uint16_t danceBeat    = 0;
uint16_t danceMisses  = 0;
uint32_t danceBeatAt  = 0;
uint16_t danceBeatMs  = 0;        // current beat interval; starts long, shortens
bool     danceShook     = false;
uint32_t danceStillStart = 0;     // millis the current still run began (0 = moving)
uint16_t danceMaxStill  = 0;      // longest still run within the current beat (ms)
// A real shake has a rest within the beat (settle→shake→settle); continuous
// shaking has none. So a beat scores only if it had both a shake AND a still run.
const uint16_t DANCE_STILL_MS = 100;
const uint16_t DANCE_BEATS = 24, DANCE_LIVES = 3;
const uint16_t DANCE_BEAT_START = 3000, DANCE_BEAT_MIN = 480, DANCE_BEAT_DEC = 110;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
// Prompt identity is tracked by CONTENT (tool+hint), not prompt.id: the
// companion regenerates the id every heartbeat for mirrored AskUserQuestion
// prompts, so an id-keyed edge re-fired the alert and reset the dismiss on
// every beat.
char     lastPromptTool[20] = "";
char     lastPromptHint[96] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     swallowBtnC = false;
bool     buddyMode = false;
bool     gifAvailable = false;
bool     clawdMode = false;          // Clawd RLE-sprite mascot (default boot persona)
bool     lowBatteryNow = false;      // coarse IP5306 low-battery flag (set by 10s poll)
const uint8_t SPECIES_GIF   = 0xFF;  // species NVS sentinel: use the installed GIF
const uint8_t SPECIES_CLAWD = 0xFE;  // species NVS sentinel: use Clawd

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
// Cycle: Clawd → ASCII species 0..N-1 → installed GIF (if any) → Clawd.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (clawdMode) {                           // Clawd → ASCII species 0
    clawdMode = false;
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddyMode) {
    if (buddySpeciesIdx() + 1 >= n) {        // last ASCII → GIF if installed, else Clawd
      buddyMode = false;
      if (gifAvailable) {
        speciesIdxSave(SPECIES_GIF);
      } else {
        clawdMode = true;
        speciesIdxSave(SPECIES_CLAWD);
      }
    } else {                                 // species i → species i+1
      buddyNextSpecies();
    }
  } else {                                   // GIF → Clawd
    clawdMode = true;
    speciesIdxSave(SPECIES_CLAWD);
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
  if (clawdMode) clawdInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;
static const uint32_t PROMPT_TIMEOUT_MS = 300000;  // 5 minutes

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() {
  int b = 20 + brightLevel * 20;
  if (settings().dnd) { b /= 2; if (b < 10) b = 10; }   // DND dims for focus
  Axp.ScreenBreath(b);
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 1600;       // length of the wake animation
    // Clawd plays a wake-up animation when the screen comes back from sleep.
    if (clawdMode) clawdTriggerScene(CLAWD_RX_WAKE, 1600);
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;
bool     promptDismissed = false;    // B hides the panel until the question changes/clears
bool     promptTimedOut = false;     // true after PROMPT_TIMEOUT_MS with no response
bool     promptPanelUp = false;      // approval panel on screen (awaiting or "sent...")

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound && !settings().dnd) Beep.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  clawdSetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "do not disturb", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 7;

// Care menu (hold-B): feed the pet, play a mini-game, or close.
const char* careItems[] = { "feed", "play catch", "play dance", "close" };
const uint8_t CARE_N = 4;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      settings().bright = brightLevel;
      applyBrightness();
      break;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* bLbl = "select") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // A = down, C = up, B = the action (select/change).
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print("A");
  spr.fillTriangle(x + 8, hy + 1, x + 14, hy + 1, x + 11, hy + 6, p.textDim);   // down
  x += 30;
  spr.setCursor(x, hy); spr.print("C");
  spr.fillTriangle(x + 8, hy + 6, x + 14, hy + 6, x + 11, hy + 1, p.textDim);   // up
  spr.setTextDatum(TR_DATUM);
  char b[16]; snprintf(b, sizeof(b), "B %s", bLbl);
  spr.drawString(b, mx + mw - 6, hy);
  spr.setTextDatum(TL_DATUM);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 180, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      // Cycle: Clawd (1) → ASCII 0..N-1 (2..N+1) → GIF (last, if installed).
      uint8_t total = buddySpeciesCount() + 1 + (gifAvailable ? 1 : 0);
      uint8_t pos   = clawdMode ? 1 : (buddyMode ? 2 + buddySpeciesIdx() : total);
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 180, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: settings().dnd = !settings().dnd; applyBrightness(); settingsSave(); break;
    case 2: Axp.PowerOff(); break;
    case 3:
    case 4:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 3) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 5: dataSetDemo(!dataDemo()); break;
    case 6: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 180, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 1) {
      spr.setTextColor(settings().dnd ? GREEN : p.textDim, PANEL);
      spr.print(settings().dnd ? "  on" : "  off");
    } else if (i == 5) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void triggerOneShot(PersonaState s, uint32_t durMs);   // defined below; used by careConfirm
void gameStart(uint8_t type);                          // defined below; launches a mini-game

void careConfirm() {
  switch (careSel) {
    case 0:  // feed. Close the menu first so the reaction is visible.
      careMenuOpen = false;
      characterInvalidate();
      if (clawdMode) clawdInvalidate();
      if (statsHunger() >= 10) {
        // Already full: no munch, no stat change (forgiving). Clawd gets briefly
        // woozy ("too stuffed") with a low playful burp. Works in every
        // character mode via the shared dizzy one-shot.
        triggerOneShot(P_DIZZY, 1500);
        PLAY_MELODY_VOL(MEL_FULL, 140);   // burp is low-pitched; boost so it carries
      } else {
        statsFeed();
        if (clawdMode) clawdTriggerScene(CLAWD_RX_FEED, 1800);
        PLAY_MELODY(MEL_FEED);
      }
      break;
    case 1:  // play catch
      careMenuOpen = false;
      characterInvalidate();
      gameStart(GAME_CATCH);
      break;
    case 2:  // play dance
      careMenuOpen = false;
      characterInvalidate();
      gameStart(GAME_DANCE);
      break;
    case 3:  // close
      careMenuOpen = false;
      characterInvalidate();
      break;
  }
}

void drawCareMenu() {
  const Palette& p = characterPalette();
  int mw = 180, mh = 16 + CARE_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < CARE_N; i++) {
    bool sel = (i == careSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(careItems[i]);
    if (i == 0) { spr.setTextColor(p.textDim, PANEL); spr.printf("   %u/10", statsHunger()); }
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = Axp.GetVBusVoltage() > 4.0f;
  dataClockNow(&_clkTm, &_clkDt);   // millis()-based software clock (Fire has no RTC)
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    // Bottom half — buddy naturally lives at y=0..82, GIF peeks at top
    // via peek mode. Clearing from 90 leaves both untouched.
    spr.fillRect(0, 90, W, H - 90, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 140);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 175);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 200);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: 240×135 direct-to-LCD. Full fill only on entry; after that
  // text glyph bg cells repaint themselves and the pet box (small, ~90×50)
  // gets a fillRect each pet tick — small enough not to tear.
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clkTm.Seconds != lastSec) {
    lastSec = _clkTm.Seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 170, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 170, 72);
                                                                  M5.Lcd.drawString(wdl, 170, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet on left at 5 fps. Clear includes the overlay-particle zone above
  // the body (y<30) — species draw Zzz/hearts there via BUDDY_Y_OVERLAY=6
  // which doesn't go through _yb, so the box has to cover it.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (clawdMode) {
      // Clawd isn't rendered on the landscape clock pet slot yet (the slot is
      // small + the LCD is rotated here, which the centered Clawd renderer
      // doesn't account for). Clear the slot and skip — the clock still shows.
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
    } else if (buddyMode) {
      // ASCII glyphs don't self-clear; wipe the box each tick. Species
      // hardcode BUDDY_X_CENTER=67 / BUDDY_Y_OVERLAY=6 for particles so
      // keep portrait coords and just swap the surface — pet lands
      // upper-left of landscape, which is where we want it anyway.
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // Full-frame GIFs paint every pixel (transparent → pal.bg), so a
      // per-tick clear just adds a visible black flash between wipe and
      // last scanline. The entry fillScreen on paintedOrient change
      // already covers the surround.
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(0);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 1)  return P_BUSY;
  return P_IDLE;   // connected, no running sessions — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// Instantaneous acceleration delta from the slow baseline. Updates the baseline.
float shakeDelta() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta;
}

bool checkShake() {
  return shakeDelta() > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

// Live seconds remaining until a usage window resets, decrementing from the
// value the companion last sent (stamped at tama.usageStampMs). -1 if unknown.
static int32_t usageResetLeft(int32_t storedSecs) {
  if (storedSecs < 0) return -1;
  uint32_t elapsed = (millis() - tama.usageStampMs) / 1000;
  int32_t left = storedSecs - (int32_t)elapsed;
  return left > 0 ? left : 0;
}
// Compact duration: "45m" / "2h" / "3d" (≥0). "?" when unknown.
static void fmtDur(int32_t secs, char* buf, size_t n) {
  if (secs < 0) { snprintf(buf, n, "?"); return; }
  int32_t m = secs / 60;
  if (m >= 1440)    snprintf(buf, n, "%ldd", (long)(m / 1440));
  else if (m >= 60) snprintf(buf, n, "%ldh", (long)(m / 60));
  else              snprintf(buf, n, "%ldm", (long)m);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A (left)");
    spr.setTextColor(p.textDim, p.bg); ln("  next screen / approve"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B (middle)");
    spr.setTextColor(p.textDim, p.bg); ln("  scroll/page / dismiss"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("C (right)");
    spr.setTextColor(p.textDim, p.bg); ln("  breathe / back / deny"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A   menu");
    spr.setTextColor(p.text, p.bg);    ln("hold B   feed & play");
    spr.setTextColor(p.textDim, p.bg); ln("  catch:B  dance:shake"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  tap=off hold=shutdn");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("USAGE");
    if (tama.sessionPct < 0 && tama.weeklyPct < 0) {
      spr.setTextColor(p.textDim, p.bg);
      ln("  companion not running");
    } else {
      char r1[8], r2[8];
      fmtDur(usageResetLeft(tama.sessionResetSecs), r1, sizeof(r1));
      fmtDur(usageResetLeft(tama.weeklyResetSecs),  r2, sizeof(r2));
      spr.setTextColor(p.textDim, p.bg);
      ln("  session  %d%% (%s)", tama.sessionPct < 0 ? 0 : tama.sessionPct, r1);
      ln("  weekly   %d%% (%s)", tama.weeklyPct  < 0 ? 0 : tama.weeklyPct,  r2);
      if (tama.usageLimited) {
        spr.setTextColor(HOT, p.bg);
        ln("  RATE LIMITED");
      }
    }
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    // The Fire's IP5306 reports a coarse level (0/25/50/75/100), -1 if
    // unreadable, plus charging state — no battery voltage/current on this board.
    int lvl = M5.Power.getBatteryLevel();
    bool charging = M5.Power.isCharging();

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    if (lvl < 0) spr.print("--"); else spr.printf("%d%%", lvl);
    spr.setTextSize(1);
    spr.setTextColor(charging ? 0x07FF : p.textDim, p.bg);
    spr.setCursor(60, y + 4);
    spr.print(charging ? "charging" : "on battery");
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  coarse level (IP5306)");
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  iram     %uKB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ln("  psram    %uKB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    ln("Rushabh Shah (Fire port)");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/rushabh91");
    ln("/claude-desktop-buddy");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("M5Stack Fire");
    ln("ESP32 + IP5306");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][56], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 88;
  const int top = H - AREA;
  spr.fillRect(0, top, W, AREA, p.bg);
  spr.drawFastHLine(0, top, W, p.textDim);

  // Header: "approve?" + remaining time countdown (red when < 30s).
  spr.setTextSize(1);
  uint32_t elapsed = millis() - promptArrivedMs;
  uint32_t remaining = elapsed < PROMPT_TIMEOUT_MS ? (PROMPT_TIMEOUT_MS - elapsed) / 1000 : 0;
  spr.setTextColor(remaining < 30 ? HOT : p.textDim, p.bg);
  spr.setCursor(6, top + 5);
  if (remaining >= 60)
    spr.printf("approve?  ~%lum", (unsigned long)((remaining + 59) / 60));
  else
    spr.printf("approve?  %lus", (unsigned long)remaining);

  // Tool name, large (fits at size 2 across the wide screen).
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 24 ? 2 : 1);
  spr.setCursor(6, top + 18);
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wrapped to up to 3 full-width lines (no mid-word truncation).
  spr.setTextColor(p.textDim, p.bg);
  static char hl[3][56];
  uint8_t hn = wrapInto(tama.promptHint, hl, 3, (W - 12) / 6);
  for (uint8_t i = 0; i < hn; i++) {
    spr.setCursor(6, top + 40 + i * 9);
    spr.print(hl[i]);
  }

  // Footer buttons (A approve / B deny — remapped to A/C in the button rework).
  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(6, H - 11);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(6, H - 11);
    spr.print("A approve");
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("B dismiss", W / 2, H - 11);
    spr.setTextColor(HOT, p.bg);
    spr.setTextDatum(TR_DATUM);
    spr.drawString("deny C", W - 6, H - 11);
    spr.setTextDatum(TL_DATUM);
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print("hunger");
  uint8_t hung = statsHunger();
  for (int i = 0; i < 10; i++) {
    int px = 54 + i * 9;
    if (i < hung) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);
  // Age (days) + bond label fill the empty space to the right of the Lv badge.
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(56, y + 1);
  spr.printf("%lud  %s", (unsigned long)statsAgeDays((uint32_t)dataEpochNow()), statsBondLabel());

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);

  // Claude usage from the companion. Dim "--" until data arrives, so a Pet
  // screen with no companion reads as content, not broken/empty.
  spr.setCursor(6, y + 50);
  if (tama.sessionPct < 0 && tama.weeklyPct < 0) {
    spr.setTextColor(p.textDim, p.bg);
    spr.print("usage    --");
  } else {
    uint16_t uc = tama.usageLimited ? HOT : p.textDim;
    spr.setTextColor(uc, p.bg);
    spr.printf("usage    S%d%% W%d%%",
               tama.sessionPct < 0 ? 0 : tama.sessionPct,
               tama.weeklyPct  < 0 ? 0 : tama.weeklyPct);
  }
  spr.setTextColor(p.textDim, p.bg);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " claude/game work");
  ln(p.textDim, " + fed + rested"); gap();

  ln(p.body,    "HUNGER");
  ln(p.textDim, " hold B to feed");
  ln(p.textDim, " drifts down slowly"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " tokens tire it");
  ln(p.textDim, " nap to refill"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "A: screens B: page");
  ln(p.textDim, "hold A: menu");
  ln(p.textDim, "hold B: feed/play");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (promptPanelUp) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int SHOW = 5, LH = 8, WIDTH = (W - 8) / 6;   // landscape: ~52 cols, 5 lines
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, H - LH - 2);
    spr.print(tama.msg);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[32][56];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(4, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

// The iconic Bluetooth rune, drawn with line segments into a ~11x16 box.
static void drawBtGlyph(int x, int y, uint16_t col) {
  int c = x + 5, t = y, b = y + 14, w = 4;
  int q1 = y + 3, q3 = y + 11;
  spr.drawLine(c, t, c, b, col);
  spr.drawLine(c - w, q1, c + w, q3, col);
  spr.drawLine(c - w, q3, c + w, q1, col);
  spr.drawLine(c + w, q1, c, t, col);
  spr.drawLine(c + w, q3, c, b, col);
}

// Persistent top status bar (drawn into the sprite): BT state, battery, clock.
// Center is intentionally left free for the session/weekly usage % (parked).
static void drawStatusBar(const Palette& p) {
  const int BAR_H = 22;
  spr.fillRect(0, 0, W, BAR_H, p.bg);

  // Bluetooth: green linked+secure, yellow linked, dim advertising, slashed off.
  bool linked = bleConnected();
  uint16_t btcol = linked ? (bleSecure() ? GREEN : 0xFFE0) : p.textDim;
  drawBtGlyph(4, 4, btcol);
  if (!settings().bt) spr.drawLine(2, 4, 14, 18, HOT);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(22, 8);
  spr.print(linked ? (dataBtActive() ? "linked" : "paired") : btName);

  // Claude usage % (from the companion), center: session + weekly. Colored by
  // the higher of the two; dim "--" until the companion provides data.
  {
    int8_t sp = tama.sessionPct, wk = tama.weeklyPct;
    int hi = sp > wk ? sp : wk;
    uint16_t uc = tama.usageLimited ? HOT
                : (hi < 0) ? p.textDim : (hi >= 90 ? HOT : hi >= 70 ? 0xFFE0 : GREEN);
    char ub[16];
    if (sp < 0 && wk < 0) strcpy(ub, "S--% W--%");
    else snprintf(ub, sizeof(ub), "S%d%% W%d%%", sp < 0 ? 0 : sp, wk < 0 ? 0 : wk);
    spr.setTextColor(uc, p.bg);
    spr.setCursor(92, 8);
    spr.print(ub);
    // After the %, a red "!" when rate-limited, else the live 5h-session reset
    // countdown ("4h"/"45m"). Fits before the DND moon (x=172) at these widths.
    if (tama.usageLimited) {
      spr.setTextColor(HOT, p.bg); spr.print(" !");
    } else if (tama.sessionResetSecs >= 0) {
      char rb[8]; fmtDur(usageResetLeft(tama.sessionResetSecs), rb, sizeof(rb));
      spr.setTextColor(p.textDim, p.bg); spr.printf(" %s", rb);
    }
  }

  // Battery: outlined cell + fill + percentage. The Fire's IP5306 only reports
  // a coarse level (0/25/50/75/100) — there's no voltage readout — and -1 if
  // it can't be read. isCharging() works, so show charge state too.
  int lvl = M5.Power.getBatteryLevel();
  bool charging = M5.Power.isCharging();
  const int bx = 198, by = 6;
  spr.drawRect(bx, by, 22, 11, p.text);
  spr.fillRect(bx + 22, by + 3, 2, 5, p.text);                 // terminal nub
  if (lvl < 0) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(bx + 28, by + 2);
    spr.print("--");
  } else {
    uint16_t bcol = charging ? 0x07FF : (lvl >= 50 ? GREEN : lvl >= 25 ? 0xFFE0 : HOT);
    spr.fillRect(bx + 2, by + 2, (18 * lvl) / 100, 7, bcol);   // charge fill
    spr.setTextColor(bcol, p.bg);
    spr.setCursor(bx + 28, by + 2);
    spr.printf("%d%%", lvl);
  }

  // Do-not-disturb crescent moon (left of the battery) when active.
  if (settings().dnd) {
    spr.fillCircle(172, 11, 6, 0xFFE0);       // soft yellow disc
    spr.fillCircle(176, 9, 6, p.bg);          // carve the crescent
  }

  // Clock (rightmost), from the millis()-seeded software clock.
  char hm[6];
  if (dataRtcValid()) snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  else                strcpy(hm, "--:--");
  spr.setTextDatum(TR_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.drawString(hm, W - 4, 8);
  spr.setTextDatum(TL_DATUM);

  spr.drawFastHLine(0, BAR_H - 1, W, p.textDim);
}

// Dedicated breathing-exercise screen, drawn into the PSRAM sprite. The buddy
// itself paces the breath: it grows on inhale, holds big at the top, shrinks on
// exhale, holds small when empty. The buddy is rendered into an offscreen
// canvas and zoom-blitted, so the scaling is smooth and flicker-free.
// Catch: the gameplay is on the LED bar (sweeping cursor + center target). The
// screen is a compact HUD — streak, best, and the result on a miss.
// Catch HUD overlay: drawn ON TOP of the live character (rendered first), so
// Clawd's idle / happy / confused reactions show through. Gameplay is on the LED
// bar (sweeping cursor + center target). Top line = streak; bottom = best + hint.
// Transparent text over the buddy's black band; bottom strip cleared to black.
static void drawGame() {
  bool dance = (gameType == GAME_DANCE);
  spr.setTextDatum(TC_DATUM);
  spr.setTextSize(1);

  char t[32];
  if (gamePhase == 0) {
    spr.setTextColor(0xFFFF);                 // white, transparent bg
    if (dance) snprintf(t, sizeof(t), "DANCE   hits %u  x%u", gameStreak, danceCombo);
    else       snprintf(t, sizeof(t), "CATCH    streak %u", gameStreak);
    spr.drawString(t, CX, 6);
    if (dance) {
      // lives (hearts) — empties show a missed beat
      int lives = (int)DANCE_LIVES - (int)danceMisses; if (lives < 0) lives = 0;
      for (int i = 0; i < DANCE_LIVES; i++)
        tinyHeart(CX - 16 + i * 16, 28, i < lives, i < lives ? GREEN : 0x4208);
      // Shake prompt: stays up for the whole beat window. "SHAKE NOW" until you
      // shake, then a confirmation until the next beat.
      if (danceBeat == 0) {
        spr.setTextColor(0x4208); spr.drawString("get ready...", CX, 184);
      } else if (danceShook && danceMaxStill >= DANCE_STILL_MS) {
        spr.setTextColor(GREEN);  spr.drawString("nice!", CX, 184);
      } else {
        spr.setTextColor(0xFFE0); spr.drawString("* SHAKE NOW *", CX, 184);
      }
    }
  } else {
    spr.setTextColor(HOT);
    if (dance) snprintf(t, sizeof(t), "DANCE DONE!  hits %u", gameStreak);
    else       snprintf(t, sizeof(t), "MISSED!   streak %u", gameStreak);
    spr.drawString(t, CX, 6);
  }

  // Bottom strip: clear to black + best/hint footer.
  spr.fillRect(0, 198, W, H - 198, 0x0000);
  char b[44];
  const char* playhint = dance ? "shake!    C quit" : "B catch   C quit";
  if (gamePhase == 0) {
    spr.setTextColor(0xAD55);                 // light gray
    snprintf(b, sizeof(b), "best %u    %s", statsGameBest(), playhint);
  } else if (gameNewBest) {
    spr.setTextColor(GREEN);
    snprintf(b, sizeof(b), "NEW BEST %u !  B again C quit", statsGameBest());
  } else {
    spr.setTextColor(0xAD55);
    snprintf(b, sizeof(b), "best %u     B again  C quit", statsGameBest());
  }
  spr.drawString(b, CX, 216);

  spr.setTextDatum(TL_DATUM);   // restore default for cursor-based drawing elsewhere
}

void gameStart(uint8_t type) {
  gameOpen     = true;
  gameType     = type;
  gamePhase    = 0;
  gameStreak   = 0;
  gameNewBest  = false;
  // catch
  gameCursor   = 0;
  gameDir      = 1;
  gameStepMs   = GAME_STEP_START;
  gameNextStep = millis();
  // dance
  danceCombo   = 0;
  danceBeat    = 0;
  danceMisses  = 0;
  danceShook   = false;
  danceStillStart = 0;
  danceMaxStill = 0;
  danceBeatMs  = DANCE_BEAT_START;
  danceBeatAt  = millis() + 1500;   // short intro before the first (slow 3s) beat
  statsAddBond(1);              // playing together deepens the bond
  statsMarkActivity();          // playing counts as activity for mood
  // idle sprite while playing: catch → walking, dance → grooving
  if (clawdMode) { clawdSetGameMode(type == GAME_DANCE ? 2 : 1); clawdInvalidate(); }
  beep(1500, 60);
}

static void drawBreath(uint32_t now) {
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
    clawdSetContext(tama.connected, lowBatteryNow, true);  // breathing scene → calm Clawd
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

void setup() {
  // The Fire's 4MB PSRAM is added to the heap but is unreliable on this SDK
  // build: allocations >4KB (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096) land in
  // PSRAM and corrupt the heap (spinlock / UART-mutex / heap-walk asserts).
  // Force every allocation into internal RAM so nothing ever touches PSRAM.
  // Must run before the first large allocation (M5.begin / BLE / sprite).
  heap_caps_malloc_extmem_enable(0xFFFFFFFFU);

  // NOTE: deliberately NOT calling Serial.begin(). M5Unified doesn't start it
  // and installing the UART0 driver here was triggering a boot-time crash on
  // this board. Without it, the firmware's Serial.* debug calls are safe
  // no-ops; the device talks to the desktop over BLE.
  M5.begin();
  // The UI is laid out for the M5StickC Plus's 135x240 portrait panel. The
  // Fire's panel has a different native orientation, so rotation 0 renders
  // the buddy sideways — rotate to bring it upright.
  M5.Lcd.setRotation(1);
  M5.Imu.begin();
  Beep.begin();
  startBt();
  ledsInit();   // SK6812 bar on GPIO15 → driven black now, killing the white glow
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  brightLevel = settings().bright;
  applyBrightness();   // re-apply now that settings (incl. DND dim and brightness) are loaded
  petNameLoad();
  buddyInit();

  // Full-screen 320x240 sprite in PSRAM. A memtest proved PSRAM is reliable
  // under BLE + sustained sprite-push, and the 135x240 PSRAM sprite was stable
  // in the full firmware — so this scales it to the whole panel. The framebuffer
  // (150KB) lives in PSRAM; the extmem guard keeps every other allocation
  // internal/DMA-safe. (Draw code still uses W=135 here — looks offset until the
  // landscape layout rework; this step only proves 320x240 PSRAM is stable.)
  spr.setPsram(true);
  spr.createSprite(320, 240);
  spritePushX = 0;
  spritePushY = 0;
  clawdInit();             // allocate the Clawd decode canvas (PSRAM)
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // Persona mode from NVS "species":
  //   0..N-1        → ASCII species index
  //   SPECIES_GIF   → installed GIF (falls back to Clawd if none installed)
  //   SPECIES_CLAWD → Clawd (also the fresh-device default; see speciesIdxLoad)
  {
    uint8_t saved = speciesIdxLoad();
    clawdMode = false;
    buddyMode = false;
    if (saved == SPECIES_GIF) {
      if (!gifAvailable) clawdMode = true;   // GIF requested but none → Clawd
      // else: GIF mode (both flags false)
    } else if (saved < buddySpeciesCount()) {
      buddyMode = true;                      // ASCII species
    } else {
      clawdMode = true;                      // SPECIES_CLAWD or unset → Clawd
    }
  }
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(spritePushX, spritePushY);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");

  // Wake-up animation on startup.
  if (clawdMode) clawdTriggerScene(CLAWD_RX_WAKE, 1600);
}

void loop() {
  M5.update();
  Beep.update();
  t++;
  uint32_t now = millis();
  melodyTick(now);

  dataPoll(&tama);

  // BLE link edge → connect / disconnect jingle.
  static bool prevConn = false;
  bool nowConn = bleConnected();
  if (nowConn && !prevConn)      PLAY_MELODY(MEL_CONNECT);
  else if (!nowConn && prevConn) PLAY_MELODY(MEL_DISCONNECT);
  prevConn = nowConn;

  if (statsPollLevelUp()) { PLAY_MELODY(MEL_LEVELUP); triggerOneShot(P_CELEBRATE, 3000); statsEnergyBoost(20); }
  // A completed task is a celebration → re-energize (edge-triggered, once per completion).
  static bool prevCompleted = false;
  if (tama.recentlyCompleted && !prevCompleted) statsEnergyBoost(20);
  prevCompleted = tama.recentlyCompleted;
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Usage-reactive animation pacing (Clawdmeter-style): the buddy animates
  // calmer when Claude usage is low and more frantic as it nears the limit.
  // 0% → 130% frame interval (slower); 100% → 70% (faster). Unknown → 100%.
  {
    int hiU = tama.sessionPct > tama.weeklyPct ? tama.sessionPct : tama.weeklyPct;
    g_animScalePct = (hiU < 0) ? 100 : (uint16_t)(130 - (hiU * 60 / 100));
  }

  // Day/night: at night Clawd winds down a notch (slower animation). Only when
  // the clock is synced; degrades to no-op otherwise.
  {
    RTC_TimeTypeDef _tm; RTC_DateTypeDef _dt;
    if (dataClockNow(&_tm, &_dt) && (_tm.Hours >= 22 || _tm.Hours < 6)) {
      uint16_t calm = g_animScalePct + 30;
      g_animScalePct = calm > 160 ? 160 : calm;
    }
  }

  // Age anchor: stamp the first valid clock reading once (persists; coarse day
  // counter on the Pet screen). Cheap no-op after it's set.
  if (statsFirstBootEpoch() == 0 && dataRtcValid()) statsStampFirstBoot((uint32_t)dataEpochNow());

  // Rate-limit reaction: when Claude is actively rate-limited, Clawd goes
  // visibly sleepy (sustained, via the scene system) and lets out a soft sigh
  // once on the rising edge. Usage unknown (companion off) → not limited → calm.
  static bool prevLimited = false;
  if (tama.usageLimited && !prevLimited) PLAY_MELODY(MEL_SIGH);
  prevLimited = tama.usageLimited;
  clawdSetSleepy(tama.usageLimited);

  // Neglect distress: when the pet is hungry, run-down, or low-mood, Clawd looks
  // overheated. Recovers as soon as you feed / let it rest.
  clawdSetUnwell(statsHunger() <= 3 || statsEnergyTier() <= 1 || statsMoodTier() == 0);

  // Hunger drifts down over time (feeding is the only way up).
  statsHungerTick(now);
  // Energy slowly regens while Claude is idle (nap is the fast refill).
  statsEnergyTick(now);

  // Hungry-and-idle attention blip: when Clawd is peckish (hunger has drifted
  // low) and Claude is idle, a gentle one-off "hey" invites care. Rate-limited
  // so it never nags. baseState==P_IDLE already excludes pending approvals/busy.
  {
    static uint32_t nextBlipAt = 0;
    if (nextBlipAt == 0) nextBlipAt = now + 120000;        // grace: no blip for ~2 min after boot
    if (clawdMode && !gameOpen && !breathOpen && !screenOff && !menuOpen && !careMenuOpen
        && baseState == P_IDLE && statsHunger() <= 4
        && (int32_t)(now - nextBlipAt) >= 0 && (int32_t)(now - oneShotUntil) >= 0) {
      clawdTriggerScene(CLAWD_RX_GREET, 1200);
      nextBlipAt = now + 240000;                            // at most once every ~4 min
    }
  }

  // Celebrate chime: ascending two-note ta-da on state entry, once per window.
  // Edge-triggered so it doesn't re-fire every loop tick during the celebrate state.
  // Routes through beep() so settings().sound gates both notes.
  static PersonaState prevActiveForChime = P_SLEEP;
  static uint32_t chimeNote2At = 0;
  if (activeState == P_CELEBRATE && prevActiveForChime != P_CELEBRATE)
    { beep(2000, 100); chimeNote2At = now + 130; }
  if (chimeNote2At && (int32_t)(now - chimeNote2At) >= 0)
    { beep(2800, 150); chimeNote2At = 0; }
  prevActiveForChime = activeState;

  // RGB LED bar. In breathing mode it paces the breath (synced to the on-screen
  // guide); otherwise it mirrors the persona as an ambient status light — off
  // when the screen is off, napping, or in do-not-disturb. Breathing is user-
  // initiated focus, so it stays lit even in DND.
  // Catch: advance the sweeping cursor (bounces at the ends), bar = cursor+target.
  if (gameOpen && gameType == GAME_CATCH) {
    if (gamePhase == 0 && (int32_t)(now - gameNextStep) >= 0) {
      gameCursor += gameDir;
      if (gameCursor >= LEDS_COUNT - 1) { gameCursor = LEDS_COUNT - 1; gameDir = -1; }
      else if (gameCursor <= 0)         { gameCursor = 0; gameDir = 1; }
      gameNextStep = now + gameStepMs;
    }
    ledsGameSet(true, gameCursor, GAME_TGT_LO, GAME_TGT_HI);
  } else if (gameOpen && gameType == GAME_DANCE) {
    // Shake Dance: a loud beep marks each beat; shake within the beat to score.
    // Beats start slow (long window) and speed up. 3 lives.
    if (gamePhase == 0) {
      // Track the longest still run this beat (a real shake leaves a rest), and
      // whether a shake spike happened. Graded together at the beat boundary, so
      // continuous shaking (no rest) misses while a deliberate shake scores.
      float d = shakeDelta();
      if (d < 0.25f) {                               // still
        if (danceStillStart == 0) danceStillStart = now;
        uint32_t run = now - danceStillStart;
        if (run > danceMaxStill) danceMaxStill = (uint16_t)(run > 65535 ? 65535 : run);
      } else {
        danceStillStart = 0;                         // moving — reset the still run
      }
      if (d > 0.9f && !danceShook) {                 // a shake spike this beat
        danceShook = true;
        if (clawdMode) clawdTriggerScene(CLAWD_RX_WIN, 500);   // Clawd looks happy when shaken
      }
      if ((int32_t)(now - danceBeatAt) >= 0) {
        if (danceBeat > 0) {                         // grade the beat that just ended
          if (danceShook && danceMaxStill >= DANCE_STILL_MS) {   // shook AND rested = real
            gameStreak++; danceCombo++;
            ledsFlash(CRGB::Green);
            beep(1400 + (danceCombo > 12 ? 12 : danceCombo) * 50, 60);
            statsEnergyBoost(1);                     // dancing energizes
          } else {
            danceMisses++; danceCombo = 0;
            ledsFlash(CRGB::Red);                    // no shake, or constant shaking → miss
          }
        }
        if (danceBeat >= DANCE_BEATS || danceMisses >= DANCE_LIVES) {
          gamePhase = 1;
          gameNewBest = statsRecordGameScore(gameStreak);
          if (clawdMode) clawdTriggerScene(gameStreak >= DANCE_BEATS/2 ? CLAWD_RX_WIN : CLAWD_RX_LOSE, 1800);
        } else {
          danceBeat++; danceShook = false; danceMaxStill = 0; danceStillStart = 0;
          if (danceBeatMs > DANCE_BEAT_MIN + DANCE_BEAT_DEC) danceBeatMs -= DANCE_BEAT_DEC;
          else danceBeatMs = DANCE_BEAT_MIN;         // ramp the tempo up over the run
          danceBeatAt = now + danceBeatMs;
          PLAY_MELODY_VOL(MEL_BEAT, (uint8_t)min(255, (int)M5.Speaker.getVolume() * 8 / 5));  // loud beat cue
        }
      }
    }
    ledsGameSet(false, 0, 0, 0);
  } else {
    ledsGameSet(false, 0, 0, 0);
  }
  clawdSetGameMode(gameOpen ? (gameType == GAME_DANCE ? 2 : 1) : 0);  // dance→grooving, catch→walking

  ledsForceBreath(breathOpen, breathStartMs);
  // A dismissed prompt should stop alerting. The pending session keeps
  // activeState at P_ATTENTION, but once B is pressed (promptDismissed, sticky)
  // the LED alert goes quiet — drop attention → idle for the bar until the
  // question changes or clears. (LED only; the on-screen buddy is unchanged.)
  PersonaState ledState =
      (promptDismissed && activeState == P_ATTENTION) ? P_IDLE : activeState;
  ledsSetState(ledState,
               settings().led && (breathOpen || (!settings().dnd && !screenOff && !napping)),
               brightLevel);
  ledsTick(now);

  // Low-battery warning on the bar (coarse IP5306 level; ledsTick's enable gate
  // already respects led/DND). Polled occasionally — getBatteryLevel is I2C.
  static uint32_t lastBattChk = 0;
  if (now - lastBattChk > 10000) {
    lastBattChk = now;
    int blvl = M5.Power.getBatteryLevel();
    lowBatteryNow = (blvl >= 0 && blvl <= 25 && !M5.Power.isCharging());
    ledsLowBattery(lowBatteryNow);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !careMenuOpen && !gameOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // New question = the tool/hint content changed (or cleared). Keyed on
  // content, not prompt.id, so the companion's per-heartbeat id churn doesn't
  // look like a fresh prompt. A genuinely new/changed question (or the prompt
  // clearing) resets the dismiss; an unchanged question stays dismissed.
  bool questionChanged = strcmp(tama.promptTool, lastPromptTool) != 0
                      || strcmp(tama.promptHint, lastPromptHint) != 0;
  if (questionChanged) {
    strncpy(lastPromptTool, tama.promptTool, sizeof(lastPromptTool)-1);
    lastPromptTool[sizeof(lastPromptTool)-1] = 0;
    strncpy(lastPromptHint, tama.promptHint, sizeof(lastPromptHint)-1);
    lastPromptHint[sizeof(lastPromptHint)-1] = 0;
    responseSent = false;
    promptDismissed = false;
    promptTimedOut = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      PLAY_MELODY(MEL_ALERT);   // approval-arrived alert
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = careMenuOpen = false;
      // Absorb any in-flight press (e.g. you were navigating Info when the
      // prompt popped) so it doesn't accidentally approve/deny what just appeared.
      swallowBtnA = swallowBtnB = swallowBtnC = true;
      // A prompt takes priority over a breathing session or a mini-game so it's
      // never missed — auto-exit either back to the approval screen.
      if (breathOpen) { breathOpen = false; M5.Lcd.fillScreen(characterPalette().bg); }
      if (gameOpen) {
        statsRecordGameScore(gameStreak);
        gameOpen = false; ledsGameSet(false, 0, 0, 0);
        M5.Lcd.fillScreen(characterPalette().bg);
      }
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      if (clawdMode) clawdInvalidate();
    }
  }

  // Dismiss is sticky: it persists until the question's content changes or
  // clears (handled by the content edge check above) — no time-based re-show.
  // Auto-dismiss after 5 minutes so a crashed bridge never leaves the panel stuck.
  if (!tama.promptId[0]) promptTimedOut = false;
  if (tama.promptId[0] && !responseSent && !promptTimedOut &&
      (int32_t)(now - promptArrivedMs) >= (int32_t)PROMPT_TIMEOUT_MS)
    promptTimedOut = true;
  bool inPrompt = tama.promptId[0] && !responseSent && !promptDismissed && !promptTimedOut;
  promptPanelUp = tama.promptId[0] && !promptDismissed && !promptTimedOut;
  // A/B/C only act on the prompt when its panel is actually visible (home).
  // On Info/Pet, the buttons keep their navigation roles (C = back to home,
  // which reveals the prompt) so you never decline something you can't see.
  bool promptShown = promptPanelUp && displayMode == DISP_NORMAL;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
      if (M5.BtnC.isPressed()) swallowBtnC = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (Axp.GetBtnPress() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      Axp.SetLDO2(false);
      screenOff = true;
    }
  }

  if (gameOpen) {
    // Game owns the buttons: C = quit; B = catch (Catch only) / play again.
    if (M5.BtnC.wasPressed() && !swallowBtnC) {
      statsRecordGameScore(gameStreak);          // bank the run on quit
      gameOpen = false;
      ledsGameSet(false, 0, 0, 0);
      beep(700, 40);
      M5.Lcd.fillScreen(characterPalette().bg);
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      if (clawdMode) clawdInvalidate();
      applyDisplayMode();
    } else if (M5.BtnB.wasPressed() && !swallowBtnB) {
      if (gamePhase == 1) {
        gameStart(gameType);                     // play again (same game)
      } else if (gameType == GAME_CATCH) {       // Catch: B is the catch button
        if (gameCursor >= GAME_TGT_LO && gameCursor <= GAME_TGT_HI) {
          gameStreak++;                          // hit: happy + speed up + rising blip
          gameStepMs = (gameStepMs > GAME_STEP_MIN + GAME_STEP_DEC)
                       ? gameStepMs - GAME_STEP_DEC : GAME_STEP_MIN;
          ledsFlash(CRGB::Green);
          beep(1500 + gameStreak * 40, 50);
          statsEnergyBoost(3);   // a happy win gives a small energy lift
          if (clawdMode) clawdTriggerScene(CLAWD_RX_WIN, 700);
        } else {
          gamePhase = 1;                         // miss: confused + game over
          gameNewBest = statsRecordGameScore(gameStreak);
          ledsFlash(CRGB::Red);
          PLAY_MELODY_VOL(MEL_DENY, (uint8_t)min(255, (int)M5.Speaker.getVolume() * 6 / 5));  // +20% louder
          if (clawdMode) clawdTriggerScene(CLAWD_RX_LOSE, 1800);
        }
      }
      // Dance while playing: B is ignored (the input is shaking).
    }
    swallowBtnA = swallowBtnB = swallowBtnC = false;
  } else if (breathOpen) {
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
    swallowBtnA = swallowBtnB = swallowBtnC = false;
  } else {
  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      careMenuOpen = false;   // the two menus are mutually exclusive
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (promptShown) {
        if (!responseSent) {
          char cmd[96];
          snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
          sendCmd(cmd);
          responseSent = true;
          uint32_t tookS = (millis() - promptArrivedMs) / 1000;
          statsOnApproval(tookS);
          PLAY_MELODY(MEL_APPROVE);
          ledsFlashApprove();
          if (tookS < 5) triggerOneShot(P_HEART, 2000);
        } else {
          promptDismissed = true;   // hide the "sent..." panel
        }
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else if (careMenuOpen) {
        beep(1800, 30);
        careSel = (careSel + 1) % CARE_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB long-press (600ms) from home opens the care menu. Mirrors BtnA's
  // press/long/release model so the long-press doesn't double-fire the short
  // action (which now lands on release, below).
  if (M5.BtnB.pressedFor(600) && !btnBLong && !swallowBtnB) {
    btnBLong = true;
    if (!promptShown && !resetOpen && !settingsOpen && !menuOpen
        && displayMode == DISP_NORMAL) {
      beep(800, 60);
      careMenuOpen = !careMenuOpen;
      careSel = 0;
      if (!careMenuOpen) characterInvalidate();
      Serial.println(careMenuOpen ? "care open" : "care close");
    }
  }
  // BtnB short (on release): dismiss prompt / confirm in a menu / page / scroll.
  if (M5.BtnB.wasReleased()) {
    if (!btnBLong && !swallowBtnB) {
      if (promptShown) {
        // Dismiss: hide the panel without deciding. Stays hidden until the
        // question changes or clears (sticky — see the content edge check).
        promptDismissed = true;
        beep(900, 40);
      } else if (resetOpen) {
        beep(2400, 30);
        applyReset(resetSel);
      } else if (settingsOpen) {
        beep(2400, 30);
        applySetting(settingsSel);
      } else if (menuOpen) {
        beep(2400, 30);
        menuConfirm();
      } else if (careMenuOpen) {
        beep(2400, 30);
        careConfirm();
      } else if (displayMode == DISP_INFO) {
        beep(2400, 30);
        infoPage = (infoPage + 1) % INFO_PAGES;
      } else if (displayMode == DISP_PET) {
        beep(2400, 30);
        petPage = (petPage + 1) % PET_PAGES;
        applyDisplayMode();
      } else {
        beep(2400, 30);
        msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
      }
    }
    btnBLong = false;
    swallowBtnB = false;
  }

  // BtnC: deny a prompt / move up in menus / back to home / enter breathing.
  if (M5.BtnC.wasPressed()) {
    if (swallowBtnC) { swallowBtnC = false; }
    else if (promptShown) {
      if (!responseSent) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        statsOnDenial();
        PLAY_MELODY(MEL_DENY);
        ledsFlashDeny();
      } else {
        promptDismissed = true;   // hide the "sent..." panel
      }
    } else if (resetOpen) {
      beep(1800, 30);
      resetSel = (resetSel + RESET_N - 1) % RESET_N;
      resetConfirmIdx = 0xFF;
    } else if (settingsOpen) {
      beep(1800, 30);
      settingsSel = (settingsSel + SETTINGS_N - 1) % SETTINGS_N;
    } else if (menuOpen) {
      beep(1800, 30);
      menuSel = (menuSel + MENU_N - 1) % MENU_N;
    } else if (careMenuOpen) {
      beep(1800, 30);
      careSel = (careSel + CARE_N - 1) % CARE_N;
    } else if (displayMode != DISP_NORMAL) {
      beep(1800, 30);
      displayMode = DISP_NORMAL;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      if (clawdMode) clawdInvalidate();
    } else {
      breathOpen = true; breathStartMs = now; breathDirty = true;
      beep(1500, 60);
    }
  }
  }  // end !breathOpen button handling

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    if (clawdMode) clawdInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (gameOpen) {
    // Render the live character (idle base) so the win/lose reactions read on
    // screen, then overlay the game HUD on top.
    if (clawdMode) { clawdSetContext(tama.connected, lowBatteryNow, false); clawdTick(P_IDLE); }
    else if (buddyMode) buddyTick(P_IDLE);
    else if (characterLoaded()) { characterSetState(P_IDLE); characterTick(); }
    else spr.fillSprite(0x0000);
    drawGame();
    spr.pushSprite(spritePushX, spritePushY);
  } else if (breathOpen) {
    drawBreath(now);
    spr.pushSprite(spritePushX, spritePushY);
  } else {
  // When an overlay (menu/settings/reset, or the taller approval panel) closes,
  // the buddy + HUD only repaint their own regions, leaving a strip of the old
  // panel behind. Clear the whole sprite once on that transition and force a
  // full buddy redraw.
  static bool wasOverlay = false;
  bool isOverlay = menuOpen || settingsOpen || resetOpen || careMenuOpen || promptShown;
  if (wasOverlay && !isOverlay) {
    spr.fillSprite(characterPalette().bg);
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    if (clawdMode) clawdInvalidate();
  }
  wasOverlay = isOverlay;

  if (napping || screenOff || landscapeClock) {
    // skip sprite render — face-down, powered off, or landscape clock
    // (which draws direct-to-LCD below)
  } else if (clawdMode) {
    clawdSetContext(tama.connected, lowBatteryNow, false);
    clawdTick(activeState);
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscapeClock) {
    drawClock();
  } else if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (displayMode == DISP_NORMAL && !blePasskey() && !clocking) drawStatusBar(characterPalette());
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    else if (careMenuOpen) drawCareMenu();
    spr.pushSprite(spritePushX, spritePushY);
  }
  }  // end else (!breathOpen)

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt && !breathOpen && !gameOpen) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    Axp.ScreenBreath(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
    // Wake-up animation when lifted from a face-down nap.
    if (clawdMode) { clawdInvalidate(); clawdTriggerScene(CLAWD_RX_WAKE, 1600); }
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb && !breathOpen && !gameOpen
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    Axp.SetLDO2(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
