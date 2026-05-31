#include "games.h"
#include "app_globals.h"
#include <M5StickCPlus.h>
#include <math.h>
#include "leds.h"
#include "melody.h"
#include "stats.h"
#include "buddy.h"
#include "buddy_clawd.h"
#include "character.h"

// ──────────────── tuning constants ────────────────
static const int8_t   GAME_TGT_LO = 4, GAME_TGT_HI = 5;
static const uint16_t GAME_STEP_START = 180, GAME_STEP_MIN = 60, GAME_STEP_DEC = 12;
// A real shake has a rest within the beat (settle→shake→settle); continuous
// shaking has none. So a beat scores only if it had both a shake AND a still run.
static const uint16_t DANCE_STILL_MS = 100;
static const uint16_t DANCE_BEATS = 24, DANCE_LIVES = 3;
static const uint16_t DANCE_BEAT_START = 3000, DANCE_BEAT_MIN = 480, DANCE_BEAT_DEC = 110;
// Stillness/shake are judged against a FIXED gravity reference (|accel|≈1g at rest),
// not the adaptive shakeDelta() baseline — see gameTick() for why.
static const float    DANCE_STILL_G = 0.12f;  // |accel|-1g below this = genuinely still
static const float    DANCE_SHAKE_G = 0.60f;  // |accel|-1g above this = a deliberate shake spike

// ──────────────── state ────────────────
static bool     gameOpen     = false;
static uint8_t  gameType     = GAME_CATCH;
static uint8_t  gamePhase    = 0;        // 0 = playing, 1 = game over
static int8_t   gameCursor   = 0;
static int8_t   gameDir      = 1;
static uint16_t gameStreak   = 0;        // score (hits) — shared "best" across both games
static uint16_t gameStepMs   = 180;      // catch: cursor step interval; shrinks as you score
static uint32_t gameNextStep = 0;
static bool     gameNewBest  = false;
// Shake Dance state.
static uint16_t danceCombo   = 0;
static uint16_t danceBeat    = 0;
static uint16_t danceMisses  = 0;
static uint32_t danceBeatAt  = 0;
static uint16_t danceBeatMs  = 0;        // current beat interval; starts long, shortens
static bool     danceShook     = false;
static uint32_t danceStillStart = 0;     // millis the current still run began (0 = moving)
static uint16_t danceMaxStill  = 0;      // longest still run within the current beat (ms)

bool gameActive() { return gameOpen; }

void gameStart(uint8_t type) {
  gameOpen     = true;
  gameType     = type;
  gamePhase    = 0;
  gameStreak   = 0;
  gameNewBest  = false;
  // catch
  gameCursor   = 0;
  gameDir      = 1;
  gameStepMs   = GAME_STEP_START;
  gameNextStep = millis();
  // dance
  danceCombo   = 0;
  danceBeat    = 0;
  danceMisses  = 0;
  danceShook   = false;
  danceStillStart = 0;
  danceMaxStill = 0;
  danceBeatMs  = DANCE_BEAT_START;
  danceBeatAt  = millis() + 1500;   // short intro before the first (slow 3s) beat
  // Re-seed the shake baseline so the post-game shake-to-dizzy path starts fresh.
  { float ax, ay, az; M5.Imu.getAccelData(&ax, &ay, &az); accelBaseline = sqrtf(ax*ax + ay*ay + az*az); }
  statsAddBond(1);              // playing together deepens the bond
  statsMarkActivity();          // playing counts as activity for mood
  // idle sprite while playing: catch → walking, dance → grooving
  if (clawdMode) { clawdSetGameMode(type == GAME_DANCE ? 2 : 1); clawdInvalidate(); }
  beep(1500, 60);
}

// Per-frame game logic + LED bar. Catch advances the sweeping cursor; Dance grades
// each beat on a shake spike + a real rest. Always updates Clawd's game mode so the
// idle sprite (grooving/walking) matches, even when no game is open.
void gameTick(uint32_t now) {
  // Catch: advance the sweeping cursor (bounces at the ends), bar = cursor+target.
  if (gameOpen && gameType == GAME_CATCH) {
    if (gamePhase == 0 && (int32_t)(now - gameNextStep) >= 0) {
      gameCursor += gameDir;
      if (gameCursor >= LEDS_COUNT - 1) { gameCursor = LEDS_COUNT - 1; gameDir = -1; }
      else if (gameCursor <= 0)         { gameCursor = 0; gameDir = 1; }
      gameNextStep = now + gameStepMs;
    }
    ledsGameSet(true, gameCursor, GAME_TGT_LO, GAME_TGT_HI);
  } else if (gameOpen && gameType == GAME_DANCE) {
    // Shake Dance: a loud beep marks each beat; shake within the beat to score.
    // Beats start slow (long window) and speed up. 3 lives.
    if (gamePhase == 0) {
      // Track the longest still run this beat (a real shake leaves a rest), and
      // whether a shake spike happened. Graded together at the beat boundary, so
      // continuous shaking (no rest) misses while a deliberate shake scores.
      // Judge stillness and shake against a FIXED gravity reference (|accel|≈1g at
      // rest), NOT the adaptive shakeDelta() baseline. The baseline drifts up under
      // sustained shaking and would log a fake "rest" mid-shake — re-opening the
      // continuous-shake loophole. Raw deviation from 1g can't be fooled that way.
      float ax, ay, az;
      M5.Imu.getAccelData(&ax, &ay, &az);
      float g = fabsf(sqrtf(ax*ax + ay*ay + az*az) - 1.0f);
      if (g < DANCE_STILL_G) {                       // genuinely still
        if (danceStillStart == 0) danceStillStart = now;
        uint32_t run = now - danceStillStart;
        if (run > danceMaxStill) danceMaxStill = (uint16_t)(run > 65535 ? 65535 : run);
      } else {
        danceStillStart = 0;                         // moving — reset the still run
      }
      if (g > DANCE_SHAKE_G && !danceShook) {         // a deliberate shake spike this beat
        danceShook = true;
        if (clawdMode) clawdTriggerScene(CLAWD_RX_WIN, 500);   // Clawd looks happy when shaken
      }
      if ((int32_t)(now - danceBeatAt) >= 0) {
        if (danceBeat > 0) {                         // grade the beat that just ended
          if (danceShook && danceMaxStill >= DANCE_STILL_MS) {   // shook AND rested = real
            gameStreak++; danceCombo++;
            ledsFlash(CRGB::Green);
            beep(1400 + (danceCombo > 12 ? 12 : danceCombo) * 50, 60);
            statsEnergyBoost(1);                     // dancing energizes
          } else {
            danceMisses++; danceCombo = 0;
            ledsFlash(CRGB::Red);                    // no shake, or constant shaking → miss
          }
        }
        if (danceBeat >= DANCE_BEATS || danceMisses >= DANCE_LIVES) {
          gamePhase = 1;
          gameNewBest = statsRecordGameScore(gameStreak);
          if (clawdMode) clawdTriggerScene(gameStreak >= DANCE_BEATS/2 ? CLAWD_RX_WIN : CLAWD_RX_LOSE, 1800);
        } else {
          danceBeat++; danceShook = false; danceMaxStill = 0; danceStillStart = 0;
          if (danceBeatMs > DANCE_BEAT_MIN + DANCE_BEAT_DEC) danceBeatMs -= DANCE_BEAT_DEC;
          else danceBeatMs = DANCE_BEAT_MIN;         // ramp the tempo up over the run
          danceBeatAt = now + danceBeatMs;
          PLAY_MELODY_VOL(MEL_BEAT, (uint8_t)min(255, (int)M5.Speaker.getVolume() * 8 / 5));  // loud beat cue
        }
      }
    }
    ledsGameSet(false, 0, 0, 0);
  } else {
    ledsGameSet(false, 0, 0, 0);
  }
  clawdSetGameMode(gameOpen ? (gameType == GAME_DANCE ? 2 : 1) : 0);  // dance→grooving, catch→walking
}

// Catch HUD overlay: drawn ON TOP of the live character (rendered first), so
// Clawd's idle / happy / confused reactions show through. Gameplay is on the LED
// bar (sweeping cursor + center target). Top line = streak; bottom = best + hint.
// Transparent text over the buddy's black band; bottom strip cleared to black.
void gameDraw() {
  bool dance = (gameType == GAME_DANCE);
  spr.setTextDatum(TC_DATUM);
  spr.setTextSize(1);

  char t[32];
  if (gamePhase == 0) {
    spr.setTextColor(0xFFFF);                 // white, transparent bg
    if (dance) snprintf(t, sizeof(t), "DANCE   hits %u  x%u", gameStreak, danceCombo);
    else       snprintf(t, sizeof(t), "CATCH    streak %u", gameStreak);
    spr.drawString(t, CX, 6);
    if (dance) {
      // lives (hearts) — empties show a missed beat
      int lives = (int)DANCE_LIVES - (int)danceMisses; if (lives < 0) lives = 0;
      for (int i = 0; i < DANCE_LIVES; i++)
        tinyHeart(CX - 16 + i * 16, 28, i < lives, i < lives ? GREEN : 0x4208);
      // Shake prompt: stays up for the whole beat window. "SHAKE NOW" until you
      // shake, then a confirmation until the next beat.
      if (danceBeat == 0) {
        spr.setTextColor(0x4208); spr.drawString("get ready...", CX, 184);
      } else if (danceShook && danceMaxStill >= DANCE_STILL_MS) {
        spr.setTextColor(GREEN);  spr.drawString("nice!", CX, 184);
      } else {
        spr.setTextColor(0xFFE0); spr.drawString("* SHAKE NOW *", CX, 184);
      }
    }
  } else {
    spr.setTextColor(HOT);
    if (dance) snprintf(t, sizeof(t), "DANCE DONE!  hits %u", gameStreak);
    else       snprintf(t, sizeof(t), "MISSED!   streak %u", gameStreak);
    spr.drawString(t, CX, 6);
  }

  // Bottom strip: clear to black + best/hint footer.
  spr.fillRect(0, 198, W, H - 198, 0x0000);
  char b[44];
  const char* playhint = dance ? "shake!    C quit" : "B catch   C quit";
  if (gamePhase == 0) {
    spr.setTextColor(0xAD55);                 // light gray
    snprintf(b, sizeof(b), "best %u    %s", statsGameBest(), playhint);
  } else if (gameNewBest) {
    spr.setTextColor(GREEN);
    snprintf(b, sizeof(b), "NEW BEST %u !  B again C quit", statsGameBest());
  } else {
    spr.setTextColor(0xAD55);
    snprintf(b, sizeof(b), "best %u     B again  C quit", statsGameBest());
  }
  spr.drawString(b, CX, 216);

  spr.setTextDatum(TL_DATUM);   // restore default for cursor-based drawing elsewhere
}

void gameButtonC() {
  statsRecordGameScore(gameStreak);          // bank the run on quit
  gameOpen = false;
  ledsGameSet(false, 0, 0, 0);
  beep(700, 40);
  M5.Lcd.fillScreen(characterPalette().bg);
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
  if (clawdMode) clawdInvalidate();
  applyDisplayMode();
}

void gameButtonB(uint32_t now) {
  if (gamePhase == 1) {
    gameStart(gameType);                     // play again (same game)
  } else if (gameType == GAME_CATCH) {       // Catch: B is the catch button
    if (gameCursor >= GAME_TGT_LO && gameCursor <= GAME_TGT_HI) {
      gameStreak++;                          // hit: happy + speed up + rising blip
      gameStepMs = (gameStepMs > GAME_STEP_MIN + GAME_STEP_DEC)
                   ? gameStepMs - GAME_STEP_DEC : GAME_STEP_MIN;
      ledsFlash(CRGB::Green);
      beep(1500 + gameStreak * 40, 50);
      statsEnergyBoost(3);   // a happy win gives a small energy lift
      if (clawdMode) clawdTriggerScene(CLAWD_RX_WIN, 700);
    } else {
      gamePhase = 1;                         // miss: confused + game over
      gameNewBest = statsRecordGameScore(gameStreak);
      ledsFlash(CRGB::Red);
      PLAY_MELODY_VOL(MEL_DENY, (uint8_t)min(255, (int)M5.Speaker.getVolume() * 6 / 5));  // +20% louder
      if (clawdMode) clawdTriggerScene(CLAWD_RX_LOSE, 1800);
    }
  }
  // Dance while playing: B is ignored (the input is shaking).
}

// Light quit used when a prompt interrupts a game: bank the run and close, but
// leave the redraw to the caller (it owns the transition to the approval screen).
void gameBankAndClose() {
  statsRecordGameScore(gameStreak);
  gameOpen = false;
  ledsGameSet(false, 0, 0, 0);
}
