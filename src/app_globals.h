#pragma once
// Cross-cutting state, geometry, colors, and helpers shared between main.cpp and
// the extracted UI modules (games, breathing, menus, screens, ...). The mutable
// objects and the helper functions are DEFINED in main.cpp; this header only
// declares them. The geometry/color constants live here directly (const at file
// scope = internal linkage, so each translation unit gets its own copy — fine for
// compile-time constants, no ODR concern).
#include <M5StickCPlus.h>   // TFT_eSprite + M5GFX global colors (GREEN, RED, ...)
#include <stdint.h>

// ── Shared layout/geometry (full landscape panel = the PSRAM sprite) ──
const int W = 320, H = 240;
const int CX = W / 2;          // 160
const int CY_BASE = 120;

// ── Shared colors used across multiple UI surfaces ──
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

// Persona state machine. Defined/driven in main.cpp; the renderers take a plain
// int, so UI modules only need the P_* values (e.g. breathing renders P_IDLE).
enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };

// ── Shared mutable state (defined in main.cpp) ──
extern TFT_eSprite spr;          // the 320x240 PSRAM render sprite
extern bool swallowBtnA, swallowBtnB, swallowBtnC;  // loop input-dispatch press guards
extern float       accelBaseline;// adaptive shake baseline (shake-to-dizzy path)
extern bool        clawdMode;    // Clawd mascot persona active
extern bool        buddyMode;    // ASCII buddy persona active

// ── Shared helpers (defined in main.cpp) ──
void beep(uint16_t freq, uint16_t dur);            // sound-gated short tone (respects DND)
void applyDisplayMode();                           // re-apply NORMAL/PET/INFO peek state
void tinyHeart(int x, int y, bool filled, uint16_t col);  // small heart glyph
