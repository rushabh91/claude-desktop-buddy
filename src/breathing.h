#pragma once
#include <stdint.h>

// Dedicated breathing-exercise mode (entered with BtnC from the home screen).
// Takes over the screen + LED bar, paces a chosen pattern, ignores screen sleep.
// BtnB cycles patterns; BtnA/BtnC exit. See breathing.cpp.
bool     breathingActive();
uint32_t breathingStartMs();                  // for the loop's ledsForceBreath() sync
void     breathingEnter(uint32_t now);        // BtnC from home
void     breathingButtons(uint32_t now);      // A/C exit (full restore) + B cycle pattern
void     breathingClose();                    // bare close (prompt-interrupt path; caller redraws)
void     breathingDraw(uint32_t now, bool connected, bool lowBattery);
