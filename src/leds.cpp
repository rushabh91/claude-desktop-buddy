#include "leds.h"

// Definitions of the shared LED-bar state declared extern in leds.h. One
// instance, linked once: FastLED.addLeds() (in ledsInit) binds to this _leds,
// and every module that calls ledsFlash/ledsGameSet/ledsSetState mutates the
// same state that main.cpp's ledsTick() renders.
CRGB     _leds[LEDS_COUNT];
uint8_t  _ledPersona     = LP_SLEEP;
bool     _ledEnabled     = true;
uint8_t  _ledBright      = 96;
uint32_t _ledFlashUntil  = 0;
CRGB     _ledFlashColor  = CRGB::Black;
bool     _ledLowBatt     = false;
uint32_t _ledLastShow    = 0;
uint8_t  _breathIdx      = 0;
bool     _breathForce    = false;
uint32_t _breathOrigin   = 0;
bool     _ledGame        = false;
int8_t   _ledGameCursor  = 0;
int8_t   _ledGameLo = 4, _ledGameHi = 5;
