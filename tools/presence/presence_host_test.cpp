// Host test for src/presence/presence_model.h and src/presence/presence_parse.h:
// the contract's payload, the unit conversion, the freshness rule and the
// smoothing - against the same numbers tools/luasim/presence_panel.py renders
// into the previews. If these two ever disagree, the previews are a lie.
//
// Built and run by tools/presence/check_presence.py. No board, no Arduino.

#include <cstdio>
#include <cstring>

#include "presence_model.h"
#include "presence_parse.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using presence::kSlots;
using presence::Report;

static presence::JsonPool g_pool;

// ── the units ───────────────────────────────────────────────────────────────
// docs/16: the feed is signed mm/s, the scene wants cm/s and tests > 12.
static void units() {
  CHECK(presence::speedCms(0) == 0);
  CHECK(presence::speedCms(240) == 24);
  CHECK(presence::speedCms(-240) == 24);    // approaching is moving, not sitting still
  CHECK(presence::speedCms(-5) == 0);
  CHECK(presence::speedCms(130) == 13);     // over the scene's 12
  CHECK(presence::speedCms(120) == 12);     // not over it
  CHECK(presence::speedCms(-2000000000) == 32767);   // clamped, not wrapped
}

// ── the contract ────────────────────────────────────────────────────────────
static void contractPayload() {
  Report r[kSlots];
  presence::Summary s;
  const char *m = "{\"t\":[[-1320,760,-240],null,null],\"p\":1,\"m\":1,\"s\":0,"
                  "\"lux\":42,\"ts\":1789567512}";
  CHECK(presence::parse(g_pool, m, strlen(m), r, &s));
  CHECK(r[0].present && r[0].x == -1320 && r[0].y == 760 && r[0].v == -240);
  CHECK(!r[1].present && !r[2].present);
  CHECK(s.have && s.people == 1 && s.moving == 1 && s.still == 0);
  CHECK(s.haveLux && s.lux == 42);
  CHECK(!s.haveOnline);
  CHECK(presence::speedCms(r[0].v) == 24);
}

static void threeTargetsAndTheSummary() {
  Report r[kSlots];
  presence::Summary s;
  const char *m = "{\"t\":[[10,20,30],[-40,50,-60],[70,80,0]],\"p\":3,\"m\":2,\"s\":1,"
                  "\"lux\":null,\"ts\":1789567600,\"online\":true}";
  CHECK(presence::parse(g_pool, m, strlen(m), r, &s));
  CHECK(r[0].present && r[1].present && r[2].present);
  CHECK(r[1].x == -40 && r[1].v == -60);
  CHECK(!s.haveLux);                       // lux is allowed to be null
  CHECK(s.haveOnline && s.online);
  CHECK(s.people == 3 && s.moving == 2 && s.still == 1);
}

static void refusals() {
  Report r[kSlots];
  const char *bad[] = {
    "",                                   // nothing
    "not json",
    "{\"p\":1}",                          // no targets array
    "{\"t\":\"nope\"}",                   // t is not an array
    "{\"t\":[[1,2,3],[4,5,6],",           // truncated
  };
  for (const char *m : bad) {
    CHECK(!presence::parse(g_pool, m, strlen(m), r, nullptr));
    for (uint8_t i = 0; i < kSlots; i++) CHECK(!r[i].present);   // nothing believable left behind
  }
  // Shapes inside a well-formed message are tolerated slot by slot.
  const char *odd = "{\"t\":[[1,2],null,[7,8,9],[99,99,99]],\"p\":1}";
  CHECK(presence::parse(g_pool, odd, strlen(odd), r, nullptr));
  CHECK(!r[0].present);                   // only two numbers: not a target
  CHECK(!r[1].present);
  CHECK(r[2].present && r[2].x == 7);     // a fourth entry cannot overrun three slots
}

static void arenaFits() {
  std::printf("  parse pool high-water %zu of %zu bytes\n", g_pool.peak(), g_pool.cap());
  CHECK(g_pool.peak() > 0);
  CHECK(g_pool.peak() <= g_pool.cap());
}

// ── the model ───────────────────────────────────────────────────────────────
static void fill(Report r[kSlots], int slot, int32_t x, int32_t y, int32_t v) {
  for (uint8_t i = 0; i < kSlots; i++) r[i] = Report{false, 0, 0, 0};
  if (slot >= 0) r[slot] = Report{true, x, y, v};
}

static void freshness() {
  presence::Model m;
  m.reset();
  Report r[kSlots];
  CHECK(m.source(1000) == presence::Source::Demo);        // nothing has arrived: the story runs

  fill(r, 0, 1000, 2000, 300);
  m.onMessage(1000, r);
  CHECK(m.source(1000) == presence::Source::Live);
  CHECK(m.target(1000, 0, nullptr, nullptr, nullptr));

  // Messages stop. The target ages out after kFreshMs, and NOT before: without
  // this the scene holds the last sample for ever - a frozen dot that cannot be
  // told from somebody sitting still (docs/16).
  CHECK(m.target(1000 + presence::kFreshMs, 0, nullptr, nullptr, nullptr));
  CHECK(!m.target(1000 + presence::kFreshMs + 1, 0, nullptr, nullptr, nullptr));
  CHECK(m.countNow(1000 + presence::kFreshMs + 1) == 0);

  // Still Live until kLostMs, then the screen is told to say so.
  CHECK(m.source(1000 + presence::kLostMs) == presence::Source::Live);
  CHECK(m.source(1000 + presence::kLostMs + 1) == presence::Source::Lost);
}

static void nullEmptiesAtOnce() {
  // The newest message decides. A slot reported null is empty now, not in five
  // seconds: the contract sends a message every 5 s while the room is empty, so
  // waiting out the age limit would race that heartbeat exactly.
  presence::Model m;
  m.reset();
  Report r[kSlots];
  fill(r, 0, 1000, 2000, 300);
  m.onMessage(1000, r);
  fill(r, -1, 0, 0, 0);
  m.onMessage(2000, r);
  CHECK(!m.target(2000, 0, nullptr, nullptr, nullptr));
  CHECK(m.countNow(2000) == 0);
  CHECK(m.source(2000) == presence::Source::Live);   // the room is empty, the feed is not gone
}

static void smoothing() {
  presence::Model m;
  m.reset();
  Report r[kSlots];
  int32_t x = 0, y = 0;
  int16_t v = 0;

  fill(r, 0, 0, 1000, 0);
  m.onMessage(1000, r);
  fill(r, 0, 1000, 1000, 500);
  m.onMessage(2000, r);

  // Drawn kLagMs behind the newest sample, so the position always falls
  // between two samples that really arrived.
  CHECK(m.target(2000, 0, &x, &y, &v) && x == 0);       // at the older sample
  CHECK(m.target(2500, 0, &x, &y, &v) && x == 500);     // halfway, interpolated
  CHECK(m.target(3000, 0, &x, &y, &v) && x == 1000);    // at the newer sample
  CHECK(v == 50);                                       // 500 mm/s -> 50 cm/s

  // Nothing more arrives: the dot STOPS where it was last really seen. It must
  // not keep gliding - that would be motion the data never reported.
  CHECK(m.target(4000, 0, &x, &y, &v) && x == 1000);
  CHECK(m.target(6000, 0, &x, &y, &v) && x == 1000);
}

static void noGlideAcrossAnAbsence() {
  // Away and back is not a walk. The sample before the absence is not a
  // predecessor of the one after it, so there is nothing to interpolate along.
  presence::Model m;
  m.reset();
  Report r[kSlots];
  int32_t x = 0, y = 0;

  fill(r, 0, 0, 1000, 0);
  m.onMessage(1000, r);
  fill(r, -1, 0, 0, 0);
  m.onMessage(2000, r);
  fill(r, 0, 4000, 1000, 0);
  m.onMessage(3000, r);
  CHECK(m.target(3000, 0, &x, &y, nullptr) && x == 4000);   // where it came back, at once
  CHECK(m.target(3500, 0, &x, &y, nullptr) && x == 4000);
}

static void mirror() {
  presence::Model m;
  m.reset();
  m.setMirror(true);
  Report r[kSlots];
  int32_t x = 0, y = 0;
  fill(r, 0, 1234, 2000, 0);
  m.onMessage(1000, r);
  CHECK(m.target(1000, 0, &x, &y, nullptr) && x == -1234 && y == 2000);   // y is never flipped
}

static void demoSetting() {
  presence::Model m;
  m.reset();
  m.setWanted(presence::kSourceDemo);
  Report r[kSlots];
  fill(r, 0, 1000, 2000, 0);
  m.onMessage(1000, r);
  CHECK(m.source(1000) == presence::Source::Demo);   // the scripted story, by choice
}

static void ring() {
  // The scene reads 14 trail steps of 0.1 s and counts at 3, 6, 9 and 12 back,
  // so the ring has to answer all of them.
  presence::Model m;
  m.reset();
  Report r[kSlots];
  uint32_t t = 1000;
  fill(r, 0, 0, 1000, 0);
  m.onMessage(t, r);
  for (int i = 0; i < 15; i++) {
    t += presence::kStepMs;
    m.tick(t);
  }
  int32_t x = 0, y = 0;
  CHECK(m.trail(0, 1, &x, &y));
  CHECK(m.trail(0, 14, &x, &y));
  CHECK(m.count(0) == 1 && m.count(12) == 1);
  CHECK(!m.trail(0, presence::kRing, &x, &y));   // past the ring, not a wrap to nonsense

  // tick() only advances on its own cadence, whatever rate loop() runs at.
  const uint8_t before = m.count(0);
  m.tick(t + 1);
  m.tick(t + 2);
  CHECK(m.count(0) == before);
}

static void bounds() {
  CHECK(presence::clampScaleM(4) == 4);
  CHECK(presence::clampScaleM(2) == 2);
  CHECK(presence::clampScaleM(6) == 6);
  CHECK(presence::clampScaleM(3) == presence::kScaleDefaultM);    // not on the dial
  CHECK(presence::clampScaleM(0) == presence::kScaleDefaultM);
  CHECK(presence::clampScaleM(99) == presence::kScaleDefaultM);
  CHECK(presence::clampSource(0) == presence::kSourceDemo);
  CHECK(presence::clampSource(1) == presence::kSourceLive);
  CHECK(presence::clampSource(7) == presence::kSourceLive);
  CHECK(presence::kRing > 14);            // the trail's deepest step
}

static void clockWrap() {
  // millis() wraps every 49.7 days; every comparison goes through since().
  presence::Model m;
  m.reset();
  Report r[kSlots];
  const uint32_t late = 0xFFFFF000u;
  fill(r, 0, 1000, 2000, 0);
  m.onMessage(late, r);
  // 0x1000 is 4096 ms: past the wrap, and still inside the freshness window.
  CHECK(m.target(late + 0x1000u, 0, nullptr, nullptr, nullptr));
  CHECK(m.source(late + 0x1000u) == presence::Source::Live);
  CHECK(!m.target(late + 6000u, 0, nullptr, nullptr, nullptr));    // ... and still ages out across it
}

int main() {
  units();
  contractPayload();
  threeTargetsAndTheSummary();
  refusals();
  arenaFits();
  freshness();
  nullEmptiesAtOnce();
  smoothing();
  noGlideAcrossAnAbsence();
  mirror();
  demoSetting();
  ring();
  bounds();
  clockWrap();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
