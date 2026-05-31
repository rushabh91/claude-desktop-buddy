#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "xfer.h"

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[16][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[96];   // wider on the landscape screen for richer detail
  int8_t   sessionPct = -1;  // Claude usage: 5h session window % (-1 = unknown)
  int8_t   weeklyPct  = -1;  // Claude usage: weekly window %
  int32_t  sessionResetSecs = -1;  // secs until 5h window resets at last push (-1 = unknown)
  int32_t  weeklyResetSecs  = -1;  // secs until weekly window resets at last push
  bool     usageLimited = false;   // true when actively rate-limited (>=100% / API status)
  uint32_t usageStampMs = 0;       // millis() when reset values arrived (for live countdown)
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Millis()-based software clock, seeded by the bridge's {"time":[epoch,tz]}
// sync. The Fire has no RTC and the shim's Rtc only round-trips values in RAM
// (it never advances), so we extrapolate local time from the sync point. The
// bridge re-sends time every heartbeat (~10s), so drift never accumulates.
static bool     _rtcValid       = false;
static time_t   _clkLocalAtSync = 0;   // local epoch (UTC + tz) at last sync
static uint32_t _clkSyncMillis  = 0;   // millis() captured at last sync
inline bool dataRtcValid() { return _rtcValid; }

// Fill tm/dt with the current local time from the software clock. Returns
// false (leaving them untouched) until the first sync arrives.
inline bool dataClockNow(RTC_TimeTypeDef* tm, RTC_DateTypeDef* dt) {
  if (!_rtcValid) return false;
  time_t local = _clkLocalAtSync + (time_t)((millis() - _clkSyncMillis) / 1000);
  struct tm lt; gmtime_r(&local, &lt);
  tm->Hours = lt.tm_hour; tm->Minutes = lt.tm_min; tm->Seconds = lt.tm_sec;
  dt->WeekDay = lt.tm_wday; dt->Month = lt.tm_mon + 1;
  dt->Date = lt.tm_mday;   dt->Year  = lt.tm_year + 1900;
  return true;
}

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    // Seed the software clock: store the local epoch and the millis() at sync.
    // dataClockNow() extrapolates forward from here.
    _clkLocalAtSync = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    _clkSyncMillis  = millis();
    extern uint32_t _clkLastRead;
    _clkLastRead = 0;   // force an immediate clock refresh in main
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  // Usage % (from the companion script): update the values and return without
  // touching session state or the pending prompt, so a usage message on a 2nd
  // BLE link never clobbers the desktop app's state/approval data.
  if (doc["session_pct"].is<int>() || doc["weekly_pct"].is<int>()) {
    if (doc["session_pct"].is<int>()) out->sessionPct = (int8_t)(int)doc["session_pct"];
    if (doc["weekly_pct"].is<int>())  out->weeklyPct  = (int8_t)(int)doc["weekly_pct"];
    bool gotReset = false;
    if (doc["session_reset"].is<int>()) { out->sessionResetSecs = (int32_t)(int)doc["session_reset"]; gotReset = true; }
    if (doc["weekly_reset"].is<int>())  { out->weeklyResetSecs  = (int32_t)(int)doc["weekly_reset"];  gotReset = true; }
    out->usageLimited = doc["rate_limited"] | false;
    if (gotReset) out->usageStampMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    // Skip oldest entries so we always store the most recent 16. Without this,
    // bridges sending > 16 entries would silently show only the oldest ones.
    uint8_t total = (uint8_t)la.size();
    uint8_t skip = total > 16 ? total - 16 : 0;
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (skip > 0) { skip--; continue; }
      if (n >= 16) break;
      const char* s = v.as<const char*>();
      strncpy(out->lines[n], s ? s : "", 91); out->lines[n][91]=0;
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
    strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    out->sessionPct = 45; out->weeklyPct = 30;   // fake usage for the status bar
    out->sessionResetSecs = 2*3600 + 900; out->weeklyResetSecs = 3*86400;
    out->usageLimited = false; out->usageStampMs = now;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    // Dummy permission prompt during the "attention" scenario so the approval
    // panel (and its alert melody / LED) can be exercised without a live bridge.
    if (s.w > 0) {
      strcpy(out->promptId,   "demo-req-1");
      strcpy(out->promptTool, "Bash");
      strcpy(out->promptHint, "rm -rf /tmp/build && make clean all");
    } else {
      out->promptId[0] = out->promptTool[0] = out->promptHint[0] = 0;
    }
    return;
  }

  _usbLine.feed(Serial, out);
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    // Clear transcript and prompt state so a quick reconnect doesn't show
    // stale lines or replay a ghost approval against an old prompt ID.
    out->nLines=0; out->promptId[0]=0; out->promptTool[0]=0; out->promptHint[0]=0;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
