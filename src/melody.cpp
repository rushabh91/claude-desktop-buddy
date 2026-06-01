#include "melody.h"

// Definitions of the shared melody playback state declared extern in melody.h.
// One instance, linked once, so any translation unit that calls melodyPlay*()
// feeds the same sequence that main.cpp's melodyTick() drains.
const Note* _melSeq    = nullptr;
uint8_t     _melLen    = 0;
uint8_t     _melIdx    = 0;
uint32_t    _melNextAt = 0;
uint8_t     _melVolSaved = 0;
