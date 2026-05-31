#include "buddy_clawd.h"
#include "buddy.h"   // g_animScalePct (usage-driven animation speed)
#include "clawd/clawd_sprites.h"
#include <esp_heap_caps.h>
#include <esp_random.h>

extern TFT_eSprite spr;   // global PSRAM framebuffer (320x240)

// ───────────────────────── sprite table ─────────────────────────
struct ClawdSprite {
  const uint16_t* rle;
  const uint32_t* off;   // frame_count + 1 word-offsets into rle
  int frames, w, h;
  float boost;           // per-sprite size multiplier on top of HOME_SCALE
};

enum {
  CL_SLEEPING, CL_IDLE, CL_ALERT, CL_HAPPY, CL_GOING_AWAY,
  CL_TYPING, CL_BUILDING, CL_JUGGLING, CL_CONDUCTING, CL_DIZZY, CL_CONFUSED,
  CL_WALKING, CL_MINI,
  CL_THINKING, CL_DEBUGGER, CL_BEACON, CL_WIZARD, CL_SWEEPING,
  CL_COUNT
};

static const ClawdSprite SPRITES[CL_COUNT] = {
  { sleeping_rle_data,   sleeping_frame_offsets,   SLEEPING_FRAME_COUNT,   SLEEPING_WIDTH,   SLEEPING_HEIGHT,   1.0f },
  { idle_rle_data,       idle_frame_offsets,       IDLE_FRAME_COUNT,       IDLE_WIDTH,       IDLE_HEIGHT,       1.0f },
  { alert_rle_data,      alert_frame_offsets,      ALERT_FRAME_COUNT,      ALERT_WIDTH,      ALERT_HEIGHT,      1.0f },
  { happy_rle_data,      happy_frame_offsets,      HAPPY_FRAME_COUNT,      HAPPY_WIDTH,      HAPPY_HEIGHT,      1.0f },
  { going_away_rle_data, going_away_frame_offsets, GOING_AWAY_FRAME_COUNT, GOING_AWAY_WIDTH, GOING_AWAY_HEIGHT, 1.0f },
  { typing_rle_data,     typing_frame_offsets,     TYPING_FRAME_COUNT,     TYPING_WIDTH,     TYPING_HEIGHT,     1.0f },
  { building_rle_data,   building_frame_offsets,   BUILDING_FRAME_COUNT,   BUILDING_WIDTH,   BUILDING_HEIGHT,   1.0f },
  { juggling_rle_data,   juggling_frame_offsets,   JUGGLING_FRAME_COUNT,   JUGGLING_WIDTH,   JUGGLING_HEIGHT,   1.0f },
  { conducting_rle_data, conducting_frame_offsets, CONDUCTING_FRAME_COUNT, CONDUCTING_WIDTH, CONDUCTING_HEIGHT, 1.0f },
  { dizzy_rle_data,      dizzy_frame_offsets,      DIZZY_FRAME_COUNT,      DIZZY_WIDTH,      DIZZY_HEIGHT,      1.0f },
  { confused_rle_data,   confused_frame_offsets,   CONFUSED_FRAME_COUNT,   CONFUSED_WIDTH,   CONFUSED_HEIGHT,   1.0f },
  { walking_rle_data,    walking_frame_offsets,    WALKING_FRAME_COUNT,    WALKING_WIDTH,    WALKING_HEIGHT,    1.3f },
  // mini-clawd is a tiny 10x7 sprite — boost it hard so it reads as a buddy, not a dot.
  { mini_crab_rle_data,  mini_crab_frame_offsets,  MINI_CRAB_FRAME_COUNT,  MINI_CRAB_WIDTH,  MINI_CRAB_HEIGHT,  6.0f },
  { thinking_rle_data,   thinking_frame_offsets,   THINKING_FRAME_COUNT,   THINKING_WIDTH,   THINKING_HEIGHT,   1.0f },
  { debugger_rle_data,   debugger_frame_offsets,   DEBUGGER_FRAME_COUNT,   DEBUGGER_WIDTH,   DEBUGGER_HEIGHT,   1.0f },
  { beacon_rle_data,     beacon_frame_offsets,     BEACON_FRAME_COUNT,     BEACON_WIDTH,     BEACON_HEIGHT,     1.0f },
  { wizard_rle_data,     wizard_frame_offsets,     WIZARD_FRAME_COUNT,     WIZARD_WIDTH,     WIZARD_HEIGHT,     1.0f },
  { sweeping_rle_data,   sweeping_frame_offsets,   SWEEPING_FRAME_COUNT,   SWEEPING_WIDTH,   SWEEPING_HEIGHT,   1.0f },
};

// ───────────────────────── scenes → pools ─────────────────────────
// A scene is the *effective* persona after context overrides. Each maps to a
// pool of one or more sprites; multi-entry pools pick a random variant per entry.
enum Scene {
  SC_SLEEP, SC_IDLE, SC_BUSY, SC_ATTENTION, SC_CELEBRATE, SC_DIZZY, SC_HEART,
  SC_DISCONNECTED, SC_LOWBATT, SC_BREATHING,
  // Sustained usage-reactive state + transient one-shot reactions (Tamagotchi layer).
  SC_SLEEPY, SC_FEEDING, SC_GAME_WIN, SC_GAME_LOSE, SC_GREET,
  SC_COUNT
};

static const uint8_t POOL_SLEEP[]      = { CL_SLEEPING };
static const uint8_t POOL_IDLE[]       = { CL_IDLE, CL_WALKING };
static const uint8_t POOL_BUSY[]       = { CL_TYPING, CL_BUILDING, CL_JUGGLING, CL_CONDUCTING, CL_WIZARD, CL_SWEEPING };
static const uint8_t POOL_ATTENTION[]  = { CL_ALERT, CL_CONFUSED, CL_THINKING, CL_DEBUGGER, CL_BEACON };
static const uint8_t POOL_CELEBRATE[]  = { CL_HAPPY };
static const uint8_t POOL_DIZZY[]      = { CL_DIZZY };
static const uint8_t POOL_HEART[]      = { CL_HAPPY };
static const uint8_t POOL_DISCONNECT[] = { CL_GOING_AWAY };
static const uint8_t POOL_LOWBATT[]    = { CL_GOING_AWAY };
static const uint8_t POOL_BREATHING[]  = { CL_IDLE };
// Tamagotchi scenes — all reuse existing sprites (zero new art / flash).
static const uint8_t POOL_SLEEPY[]     = { CL_SLEEPING };   // rate-limited / low energy
static const uint8_t POOL_FEEDING[]    = { CL_HAPPY };
static const uint8_t POOL_GAMEWIN[]    = { CL_HAPPY };
static const uint8_t POOL_GAMELOSE[]   = { CL_DIZZY };
static const uint8_t POOL_GREET[]      = { CL_HAPPY };

struct Pool { const uint8_t* items; uint8_t n; };
static const Pool POOLS[SC_COUNT] = {
  { POOL_SLEEP, 1 }, { POOL_IDLE, 2 }, { POOL_BUSY, 6 }, { POOL_ATTENTION, 5 },
  { POOL_CELEBRATE, 1 }, { POOL_DIZZY, 1 }, { POOL_HEART, 1 },
  { POOL_DISCONNECT, 1 }, { POOL_LOWBATT, 1 }, { POOL_BREATHING, 1 },
  { POOL_SLEEPY, 1 }, { POOL_FEEDING, 1 }, { POOL_GAMEWIN, 1 }, { POOL_GAMELOSE, 1 }, { POOL_GREET, 1 },
};

// Mirrors PersonaState in main.cpp: 0=sleep 1=idle 2=busy 3=attention
// 4=celebrate 5=dizzy 6=heart.
// showDisconnect is a transient flag (set for a few seconds when the link
// actually drops), NOT "currently disconnected" — so a never-connected boot
// rests on idle instead of looping the going_away animation forever.
//
// `sleepy` is a sustained context state (rate-limited / low energy). Precedence:
// breathing > disconnect > low-battery > approval(attention) > sleepy > persona.
// Approval outranks sleepy so a pending permission is never hidden behind a nap;
// disconnect/low-battery stay above approval as before (link/power are critical).
static Scene sceneFor(uint8_t persona, bool showDisconnect, bool lowBatt, bool sleepy, bool breathing) {
  if (breathing)      return SC_BREATHING;
  if (showDisconnect) return SC_DISCONNECTED;
  if (lowBatt)        return SC_LOWBATT;
  if (persona == 3)   return SC_ATTENTION;   // approval pending beats sleepy
  if (sleepy)         return SC_SLEEPY;
  switch (persona) {
    case 0: return SC_SLEEP;
    case 2: return SC_BUSY;
    case 4: return SC_CELEBRATE;
    case 5: return SC_DIZZY;
    case 6: return SC_HEART;
    case 1:
    default: return SC_IDLE;
  }
}

// ───────────────────────── layout ─────────────────────────
static const uint16_t CLAWD_BG     = 0x0000;   // home/breath background (black)
static const int      PEEK_TOP     = 70;        // INFO/PET header strip height
static const int      HOME_CY      = 108;       // vertical center of the home buddy band (22..196)
static const int      HOME_CLEAR_H = 196;       // clear band: status bar (0..22) to HUD top (196)
static const float    HOME_SCALE   = 1.3f;      // home render zoom (sprites are smallish)
static const uint32_t FRAME_MS     = 80;        // ~12.5 fps
// Transient disconnect reaction: play going_away briefly when the link drops,
// then fall back to the resting persona.
static const uint32_t DISCONNECT_SHOW_MS = 3000;

// ───────────────────────── state ─────────────────────────
static M5Canvas  clawdCanvas(&M5.Lcd);   // reusable decode surface (PSRAM)
static bool      ready          = false;
static bool      peek           = false;
static bool      g_lowBatt      = false;
static bool      g_breathing    = false;
static bool      g_sleepy       = false;   // sustained: rate-limited / low energy
static bool      prevConnected  = false;   // edge-detect link drops
static uint32_t  disconnectUntil = 0;      // going_away shows until this millis()
static int       reactionScene   = SC_IDLE; // active transient reaction scene
static uint32_t  reactionUntil   = 0;       // reaction shows until this millis()
static int       lastScene   = -1;
static int       curSprite   = CL_IDLE;
static int       curFrame    = 0;
static uint32_t  nextFrameAt = 0;
static bool      forceRedraw = true;

void clawdInit() {
  clawdCanvas.setColorDepth(16);
  clawdCanvas.setPsram(true);
  if (!clawdCanvas.createSprite(CLAWD_MAX_W, CLAWD_MAX_H)) return;
  clawdCanvas.setPivot(CLAWD_MAX_W / 2.0f, CLAWD_MAX_H / 2.0f);
  ready = true;
}

void clawdSetContext(bool connected, bool lowBattery, bool breathing) {
  // Fire the going_away reaction only on a real connected→disconnected edge.
  // prevConnected starts false, so a never-connected boot never triggers it.
  if (prevConnected && !connected) disconnectUntil = millis() + DISCONNECT_SHOW_MS;
  prevConnected = connected;
  g_lowBatt   = lowBattery;
  g_breathing = breathing;
}

void clawdSetPeek(bool p) {
  if (peek == p) return;
  peek = p;
  forceRedraw = true;
}

void clawdInvalidate() {
  lastScene   = -1;   // force scene re-eval + variant re-pick
  forceRedraw = true;
}

void clawdSetSleepy(bool sleepy) { g_sleepy = sleepy; }

void clawdTriggerScene(uint8_t reaction, uint16_t durationMs) {
  switch (reaction) {
    case CLAWD_RX_FEED:  reactionScene = SC_FEEDING;   break;
    case CLAWD_RX_WIN:   reactionScene = SC_GAME_WIN;  break;
    case CLAWD_RX_LOSE:  reactionScene = SC_GAME_LOSE; break;
    case CLAWD_RX_GREET: reactionScene = SC_GREET;     break;
    default: return;
  }
  reactionUntil = millis() + durationMs;
}

// Decode one RLE frame, centered, into the fixed-size canvas. The border
// around the (smaller) frame is filled with the transparent key so a single
// centered pushRotateZoom places + keys the sprite uniformly at any scale.
//
// Pixels go through drawPixel() (the LGFX color pipeline), NOT a raw write to
// getBuffer(): the sprite stores 16-bit pixels in a swapped byte order, so a
// raw uint16 write renders with R/B swapped (orange→cyan) and the transparent
// key byte-swapped (0x18C5→0xC518, a pink) so it never keys out. drawPixel
// takes logical RGB565 and handles the storage order — same path the GIF
// renderer uses. Transparent runs are skipped (the fill already covers them).
// Warm the palette toward orange: boost red, trim blue, leave green. Clawd is
// an orange crab; this pushes the whole sprite warmer without touching the
// (skipped) transparent key. Tunable via the >>2 shift (≈25%).
static inline uint16_t warmify(uint16_t v) {
  uint16_t r = (v >> 11) & 0x1F;
  uint16_t g = (v >> 5)  & 0x3F;
  uint16_t b =  v        & 0x1F;
  r += (r >> 2);              // +25% red
  if (r > 31) r = 31;
  b -= (b >> 2);              // -25% blue
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void decodeCentered(const ClawdSprite& s, int frame) {
  clawdCanvas.fillSprite(CLAWD_TRANSPARENT_KEY);
  const int ox = (CLAWD_MAX_W - s.w) / 2;
  const int oy = (CLAWD_MAX_H - s.h) / 2;
  const uint16_t* p = s.rle + s.off[frame];
  const int n = s.w * s.h;
  int pos = 0, x = 0, y = 0;
  while (pos < n) {
    uint16_t v = *p++;
    uint16_t c = *p++;
    if (v != CLAWD_TRANSPARENT_KEY) {
      uint16_t w = warmify(v);   // one conversion per run
      while (c-- && pos < n) {
        clawdCanvas.drawPixel(ox + x, oy + y, w);
        pos++;
        if (++x == s.w) { x = 0; ++y; }
      }
    } else {
      while (c-- && pos < n) { pos++; if (++x == s.w) { x = 0; ++y; } }
    }
  }
}

// Core render. toHome → into global spr with tick gating + band clear.
// Otherwise → into an arbitrary surface (breath/clock), full redraw each call.
static void render(TFT_eSprite* dst, uint8_t persona, bool toHome) {
  if (!ready) return;
  uint32_t now = millis();

  bool showDisconnect = (int32_t)(now - disconnectUntil) < 0;
  Scene base = sceneFor(persona, showDisconnect, g_lowBatt, g_sleepy, g_breathing);
  // Precedence: approval / breathing / battery / disconnect outrank a transient
  // reaction; a reaction overrides only the calm/persona scenes (idle, sleepy,
  // busy, celebrate, ...). An approval cancels a playing reaction so it can't
  // resume and bury the next prompt.
  bool reactionActive = (int32_t)(now - reactionUntil) < 0;
  if (base == SC_ATTENTION) { reactionActive = false; reactionUntil = 0; }
  Scene sc;
  if (reactionActive && base != SC_BREATHING && base != SC_LOWBATT && base != SC_DISCONNECTED)
    sc = (Scene)reactionScene;
  else
    sc = base;
  bool sceneChanged = ((int)sc != lastScene);
  if (sceneChanged) {
    lastScene = (int)sc;
    const Pool& pl = POOLS[sc];
    curSprite = pl.items[pl.n > 1 ? (int)(esp_random() % pl.n) : 0];
    curFrame  = 0;
    nextFrameAt = 0;   // draw immediately
  }

  bool advance = (int32_t)(now - nextFrameAt) >= 0;
  // Home path gates redraws to the frame clock; off-screen targets always draw.
  if (toHome && !advance && !sceneChanged && !forceRedraw) return;

  const ClawdSprite& s = SPRITES[curSprite];
  if (advance) {
    if (!sceneChanged) curFrame = (curFrame + 1) % s.frames;
    uint32_t fms;
    if (sc == SC_SLEEPY) {
      fms = 200;   // calm sleep: ignore the usage-driven frantic pacing while sleepy
    } else {
      fms = (uint32_t)FRAME_MS * g_animScalePct / 100;
      if (fms < 40)  fms = 40;
      if (fms > 160) fms = 160;
    }
    nextFrameAt = now + fms;
  }
  forceRedraw = false;

  decodeCentered(s, curFrame);

  // Home: scale up + center in the 22..196 band. Peek: half scale in the top
  // strip. Off-screen targets (breath/clock) render at 1x — the caller owns sizing.
  float scale;
  int cx, cy;
  if (!toHome) {
    scale = 1.0f;
    cx = dst->width() / 2;
    cy = dst->height() / 2;
    dst->fillScreen(CLAWD_BG);
  } else {
    scale = (peek ? 0.5f : HOME_SCALE) * s.boost;
    cx = spr.width() / 2;
    cy = peek ? (PEEK_TOP / 2) : HOME_CY;
    spr.fillRect(0, 0, spr.width(), peek ? PEEK_TOP : HOME_CLEAR_H, CLAWD_BG);
  }
  clawdCanvas.pushRotateZoom(dst, cx, cy, 0.0f, scale, scale, CLAWD_TRANSPARENT_KEY);
}

void clawdTick(uint8_t personaState) {
  render(&spr, personaState, true);
}

void clawdRenderTo(TFT_eSprite* tgt, uint8_t personaState) {
  render(tgt, personaState, false);
}
