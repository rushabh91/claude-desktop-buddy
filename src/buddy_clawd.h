#pragma once
#include <stdint.h>
#include <M5StickCPlus.h>   // compat shim: TFT_eSPI / TFT_eSprite

// Clawd: the Claude Code mascot, rendered from compiled-in RLE RGB565 sprites
// (vendored under src/clawd/). A third buddy mode alongside the ASCII species
// and the LittleFS GIF characters. Selected as the default boot persona.
//
// Persona states map to a sprite pool; pools with >1 entry pick a random
// variant on each state entry so repeated states don't look identical.
// Connection / battery / breathing context can override the persona to show
// dedicated scenes (disconnected, low-battery, breathing-calm).

// Allocate the reusable decode canvas in PSRAM. Call once after M5.begin()
// and the main sprite's createSprite().
void clawdInit();

// Feed runtime context so the renderer can pick context scenes. Call each
// loop before clawdTick(). lowBattery / breathing override the persona.
void clawdSetContext(bool connected, bool lowBattery, bool breathing);

// Decode + blit the current frame into the global `spr` at the home (or peek)
// position. Animation-timed internally; cheap to call every loop iteration.
void clawdTick(uint8_t personaState);

// One-shot render to an arbitrary sprite (breathing screen / landscape clock),
// centered, scale 1. Advances animation so it animates when clawdTick is bypassed.
void clawdRenderTo(TFT_eSprite* tgt, uint8_t personaState);

// Peek mode (INFO/PET panel header): render at half scale in the top strip.
void clawdSetPeek(bool peek);

// Force a full redraw + variant re-pick on the next tick (mode switch, overlay close).
void clawdInvalidate();

// Sustained "sleepy" state (rate-limited / low energy). Set each loop; persists
// for exactly as long as the state holds. Outranks the resting persona but
// yields to a pending approval (P_ATTENTION), low battery, and link-drop scenes.
void clawdSetSleepy(bool sleepy);

// Transient one-shot reactions: play for durationMs, then fall back to the
// persona/context scene. A pending approval (P_ATTENTION) takes precedence and
// cancels a playing reaction so the device never buries an approval behind a toy.
enum ClawdReaction { CLAWD_RX_FEED = 0, CLAWD_RX_WIN, CLAWD_RX_LOSE, CLAWD_RX_GREET };
void clawdTriggerScene(uint8_t reaction, uint16_t durationMs);
