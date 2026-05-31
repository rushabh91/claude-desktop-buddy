#pragma once
#include <stdint.h>

// Multi-species ASCII buddy renderer. Each species lives in its own
// src/buddies/<name>.cpp file and exposes 7 state functions matching
// the PersonaState enum order: sleep, idle, busy, attention, celebrate,
// dizzy, heart.
void buddyInit();
void buddyTick(uint8_t personaState);
void buddyInvalidate();

// Animation-speed scale in percent (100 = normal). main.cpp sets this from
// Claude usage so the buddy animates calmer when usage is low and more frantic
// as it nears the limit. Renderers multiply their frame interval by this/100.
extern uint16_t g_animScalePct;
#include <M5StickCPlus.h>   // compat shim: provides TFT_eSPI / TFT_eSprite
void buddyRenderTo(TFT_eSPI* tgt, uint8_t personaState);
void buddySetSpecies(const char* name);
void buddySetSpeciesIdx(uint8_t idx);
void buddyNextSpecies();
void buddySetPeek(bool peek);
uint8_t buddySpeciesIdx();
uint8_t buddySpeciesCount();
const char* buddySpeciesName();

// Per-species state function: takes the global tickCount and renders
// the buddy + any overlays for the current state into the shared sprite.
typedef void (*StateFn)(uint32_t t);

struct Species {
  const char* name;
  uint16_t bodyColor;
  StateFn states[7];   // index by PersonaState (0=sleep .. 6=heart)
};
