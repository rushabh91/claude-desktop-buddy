#pragma once
// Clawd RLE sprite set, vendored from marciogranzotto/clawd-tank (MIT).
// See src/clawd/NOTICE for attribution. Each header defines:
//   <name>_rle_data[]      : uint16_t (value,count) RLE pairs, all frames packed
//   <name>_frame_offsets[] : uint32_t word-offsets into rle_data, frame_count+1 entries
//   <NAME>_WIDTH/_HEIGHT/_FRAME_COUNT : geometry
// All sprites share transparent key 0x18C5 (#1a1a2e).

#include <stdint.h>

#include "sprite_sleeping.h"
#include "sprite_idle.h"
#include "sprite_alert.h"
#include "sprite_happy.h"
#include "sprite_going_away.h"
#include "sprite_typing.h"
#include "sprite_building.h"
#include "sprite_juggling.h"
#include "sprite_conducting.h"
#include "sprite_dizzy.h"
#include "sprite_confused.h"
#include "sprite_walking.h"
#include "sprite_mini_crab.h"
#include "sprite_thinking.h"
#include "sprite_debugger.h"
#include "sprite_beacon.h"
#include "sprite_wizard.h"
#include "sprite_sweeping.h"
// Tamagotchi-layer animations (also vendored from clawd-tank via the same tools).
#include "sprite_eureka.h"
#include "sprite_grooving.h"
#include "sprite_hat_mishap.h"
#include "sprite_overheated.h"
#include "sprite_wake.h"

// Shared transparent key across all Clawd sprites.
#define CLAWD_TRANSPARENT_KEY 0x18C5

// Largest sprite footprint — sizes the reusable decode canvas. Must cover the
// widest (going_away, 170) and tallest (wizard, 129) sprites; a frame larger
// than this overflows the canvas. Bump if you add a bigger sprite.
#define CLAWD_MAX_W 170
#define CLAWD_MAX_H 129
