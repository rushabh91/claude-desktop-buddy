#include "stats.h"

// Definitions of the shared stats / settings / runtime state declared extern in
// stats.h. One instance, linked once, so every module sees the same scores,
// settings, energy meter, and NVS handle. (Static-storage globals are zero-
// initialized; statsLoad()/settingsLoad() populate them from NVS at boot.)
Stats       _stats;
Preferences _prefs;
bool        _dirty = false;

uint32_t _lastBridgeTokens  = 0;
bool     _tokensSynced      = false;
bool     _levelUpPending    = false;
uint32_t _lastActivityMs    = 0;
uint32_t _lastHungerDriftMs = 0;
int16_t  _energyPts         = 60;
uint32_t _lastTokenMs       = 0;
uint32_t _lastEnergyRegenMs = 0;

Settings _settings = { true, true, false, true, true, false, 0, 4 };

char _petName[24]  = "Buddy";
char _ownerName[32] = "";
