#pragma once
#include <stdint.h>

// Two mini-games, launched from the care menu. Self-contained screen + LED bar,
// like breathing mode. C = quit; B = catch / play-again.
//   GAME_CATCH — LED cursor sweeps, press B at the center target.
//   GAME_DANCE — shake the device on each audible beat (IMU rhythm); see games.cpp.
enum { GAME_CATCH = 0, GAME_DANCE = 1 };

void gameStart(uint8_t type);     // launch a game
bool gameActive();                // a game is currently open
void gameTick(uint32_t now);      // per-frame cursor/beat logic + LED bar (call every loop)
void gameDraw();                  // overlay HUD on top of the already-rendered character
void gameButtonB(uint32_t now);   // B pressed while a game is open (catch / play-again)
void gameButtonC();               // C pressed = quit (banks the run, restores the screen)
void gameBankAndClose();          // bank the run + close, no redraw (prompt-interrupt path)
