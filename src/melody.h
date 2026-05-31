#pragma once
#include <M5Unified.h>
#include "stats.h"   // settings().sound

// ───────────────────────── Non-blocking melody engine ───────────────────────
// The Fire's I2S speaker (M5.Speaker) can play tone sequences, but its per-
// channel queue is only 2 deep, so we feed one note per tick from loop()
// instead of enqueueing a whole melody at once. Single-tone UI clicks still go
// through beep() in main.cpp; only the event sounds below are melodies.
// All tables live in flash (zero RAM). Everything gates on settings().sound.

struct Note { uint16_t freq; uint16_t ms; };   // freq == 0 → rest

static const Note* _melSeq    = nullptr;
static uint8_t     _melLen    = 0;
static uint8_t     _melIdx    = 0;
static uint32_t    _melNextAt = 0;

// Event melodies (kept short; ≤ ~5 notes).
static const Note MEL_CONNECT[]    = {{784, 90}, {1047, 90}, {1319, 130}};            // G5-C6-E6 rising
static const Note MEL_DISCONNECT[] = {{659, 90}, {494, 140}};                         // E5-B4 falling
static const Note MEL_ALERT[]      = {{1200, 80}, {0, 50}, {1500, 100}};              // approval arrived
static const Note MEL_APPROVE[]    = {{2000, 60}, {2600, 110}};                       // bright up
static const Note MEL_DENY[]       = {{700, 80}, {500, 140}};                         // low down
static const Note MEL_LEVELUP[]    = {{784, 80}, {988, 80}, {1175, 80}, {1568, 180}}; // arpeggio
static const Note MEL_SIGH[]       = {{392, 180}, {294, 320}};                        // G4-D4 soft descending — rate-limited "sigh"

inline void melodyStop() { _melSeq = nullptr; _melLen = 0; _melIdx = 0; }

inline void melodyPlay(const Note* seq, uint8_t len) {
  if (!settings().sound || settings().dnd) return;
  _melSeq = seq; _melLen = len; _melIdx = 0; _melNextAt = millis();
}
// Play a melody table without hand-counting its length.
#define PLAY_MELODY(arr) melodyPlay((arr), (uint8_t)(sizeof(arr) / sizeof((arr)[0])))

inline void melodyTick(uint32_t now) {
  if (!_melSeq || _melIdx >= _melLen) { _melSeq = nullptr; return; }
  if ((int32_t)(now - _melNextAt) < 0) return;
  const Note& n = _melSeq[_melIdx++];
  if (n.freq) M5.Speaker.tone((float)n.freq, (uint32_t)n.ms);
  _melNextAt = now + n.ms;
}
