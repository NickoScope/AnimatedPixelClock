// fx3d.cpp - 3D on the panel: the scenes' and looks' buffers in PSRAM, the
// blit, the render tick's hooks, /api/fx3d, and the bench. The arithmetic
// lives in the headers next to this file and is tested on the host
// (tools/fx3d/check_fx3d.py).
#include "fx3d.h"

#if defined(FX3D_ENABLED)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <time.h>

#include "../display/display.h"
#include "../web/web.h"
#include "fx3d_catalog.h"
#include "fx3d_page.h"
#include "fx3d_present.h"

using namespace fx3d;

static_assert(HUB75_PANEL_W * HUB75_CHAIN == kW && HUB75_PANEL_H == kH,
              "fx3d draws 128 x 64: the capture and the model must match the panel's chain");

namespace {

// Everything sizeable is in PSRAM: the frame (colour, two eye planes, depth)
// and the encoder in one block; the scene on screen; and, while a look is on,
// the page's captured frame and the look itself. Internal RAM holds only these
// statics.
uint8_t *g_block = nullptr;
Encoder *g_enc = nullptr;
Ctx g_ctx;
void *g_sceneMem = nullptr;
Scene *g_scene = nullptr;
int g_sceneIdx = -1;
uint8_t *g_capture = nullptr;      // kPixels * 3 codes, what the page drew
PictureScene *g_pic = nullptr;     // the look
uint8_t g_look = LOOK_FLAT;
uint32_t g_lastUs = 0;
bool g_haveLast = false;
const uint32_t kSeed = 20260918u;
const int kSceneHz = 30;   // the ambient rate in getOptimalRefreshRate() (main.cpp)

// One run's timings, for the bench and for /api/fx3d.
struct Run {
  uint32_t frames, renderSum, renderMax, blitSum, blitMax, openUs;
  uint32_t firstMs, lastMs, heapMin;
};
Run g_run;
const uint32_t kWarmupFrames = 15;   // the first half second is settling, not cost
uint32_t g_seen = 0;

void *psram(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

uint8_t *g_row = nullptr;   // one row of codes for the blit, in the block below

bool allocBlock() {
  g_block = (uint8_t *)psram(kFrameBytes + sizeof(Encoder) + kW * 3);
  if (!g_block) return false;
  uint8_t *p = g_block;
  g_ctx.fb.rgb = p;
  p += kPixels * 3;
  g_ctx.fb.eye[0] = p;
  p += kPixels;
  g_ctx.fb.eye[1] = p;
  p += kPixels;
  g_ctx.fb.z = reinterpret_cast<float *>(p);   // offset 40960: four-byte aligned
  p += sizeof(float) * kPixels;
  g_enc = new (p) Encoder();
  p += sizeof(Encoder);
  g_row = p;
  return true;
}

void resetRun() {
  memset(&g_run, 0, sizeof g_run);
  g_run.heapMin = UINT32_MAX;
  g_seen = 0;
  g_haveLast = false;
}

// Seconds since the last frame, from an unsigned difference; a stall becomes
// a step, not a jump.
float frameDt() {
  const uint32_t now = micros();
  float dt = g_haveLast ? (float)(now - g_lastUs) * 1.0e-6f : 0.0f;
  if (dt > 0.1f) dt = 0.1f;
  g_lastUs = now;
  g_haveLast = true;
  return dt;
}

void fillEnv(Env &w) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const time_t now = tv.tv_sec;
  struct tm lt, ut;
  localtime_r(&now, &lt);
  gmtime_r(&now, &ut);
  w.valid = lt.tm_year > 120;   // the rule peekLocalTime() in main.cpp uses
  w.hour = lt.tm_hour;
  w.minute = lt.tm_min;
  w.second = lt.tm_sec;
  w.sub = (float)tv.tv_usec * 1.0e-6f;
  w.yday = ut.tm_yday;
  w.utcHours = (float)ut.tm_hour + (float)ut.tm_min / 60.0f + (float)ut.tm_sec / 3600.0f;
}

// Linear light to the library's CIE codes, past any capture, a run of one
// colour at a time (forEachRun in fx3d_model.h says why). Every pixel of the
// frame is written, as before.
void blit() {
  const uint8_t *px = g_ctx.fb.rgb;
  const uint8_t *code = g_enc->code;
  for (int y = 0; y < kH; y++) {
    for (int i = 0; i < kW * 3; i++) g_row[i] = code[px[i]];
    px += kW * 3;
    forEachRun(g_row, [y](int x, int n, const uint8_t *c) {
      if (n == 1) display.panelPixelRGB888((int16_t)x, (int16_t)y, c[0], c[1], c[2]);
      else display.panelHLineRGB888((int16_t)x, (int16_t)y, (int16_t)n, c[0], c[1], c[2]);
    });
  }
}

void note(uint32_t renderUs, uint32_t blitUs) {
  if (++g_seen <= kWarmupFrames) return;
  const uint32_t now = millis();
  if (!g_run.frames) g_run.firstMs = now;
  g_run.lastMs = now;
  g_run.frames++;
  g_run.renderSum += renderUs;
  g_run.blitSum += blitUs;
  if (renderUs > g_run.renderMax) g_run.renderMax = renderUs;
  if (blitUs > g_run.blitMax) g_run.blitMax = blitUs;
  const uint32_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (heap < g_run.heapMin) g_run.heapMin = heap;
}

// ---- the look: any page's frame in 3D ----

// Called by display.display() while the capture is on.
void presentLook() {
  if (!g_pic || !g_block) return;
  g_pic->codes = g_capture;
  Env env;
  fillEnv(env);
  const float dt = frameDt();
  const uint32_t t0 = micros();
  renderFrame(*g_pic, g_ctx, dt, env);
  const uint32_t t1 = micros();
  blit();
  note(t1 - t0, micros() - t1);
}

// The capture runs while a look other than flat is on and no scene owns the screen.
void updateCapture() {
  const bool on = g_look != LOOK_FLAT && g_pic && g_capture && !g_scene;
  display.capture(on ? g_capture : nullptr, on ? &presentLook : nullptr);
}

bool setLook(uint8_t look) {
  if (look == LOOK_FLAT) {
    g_look = LOOK_FLAT;
    updateCapture();
    if (g_pic) {
      g_pic->~PictureScene();
      heap_caps_free(g_pic);
      g_pic = nullptr;
    }
    if (g_capture) {
      heap_caps_free(g_capture);
      g_capture = nullptr;
    }
    return true;
  }
  if (!g_capture) g_capture = (uint8_t *)psram((size_t)kPixels * 3);
  if (!g_pic) {
    void *m = psram(sizeof(PictureScene));
    if (m) g_pic = new (m) PictureScene();
  }
  if (!g_capture || !g_pic) {
    Serial.printf("[fx3d] no PSRAM for a look (%u B)\n", (unsigned)(kPixels * 3 + sizeof(PictureScene)));
    setLook(LOOK_FLAT);
    return false;
  }
  memset(g_capture, 0, (size_t)kPixels * 3);
  g_pic->look = look;
  g_pic->reset(kSeed);
  g_look = look;
  resetRun();
  updateCapture();
  return true;
}

// ---- the scene: a page of its own ----

void closeScene() {
  if (g_scene) {
    g_scene->~Scene();
    g_scene = nullptr;
  }
  if (g_sceneMem) {
    heap_caps_free(g_sceneMem);
    g_sceneMem = nullptr;
  }
  g_sceneIdx = -1;
  resetRun();
  updateCapture();
}

bool openScene(int idx) {
  closeScene();
  resetRun();
  const CatalogEntry &e = kCatalog[idx];
  const uint32_t t0 = micros();
  g_sceneMem = psram(e.bytes);
  if (!g_sceneMem) {
    Serial.printf("[fx3d] no PSRAM for %s (%u B)\n", e.id, (unsigned)e.bytes);
    return false;
  }
  g_scene = e.make(g_sceneMem);
  g_scene->reset(kSeed);
  g_run.openUs = micros() - t0;
  g_sceneIdx = idx;
  updateCapture();   // a scene draws itself: the look waits
  return true;
}

// ---- reporting ----

const char *modeName(uint8_t m) {
  static const char *const names[MODE_COUNT] = {"mono", "redblue", "redcyan", "redgreen"};
  return m < MODE_COUNT ? names[m] : "?";
}
int modeByName(const String &s) {
  for (int m = 0; m < MODE_COUNT; m++)
    if (s == modeName((uint8_t)m)) return m;
  return -1;
}
int lookByName(const String &s) {
  if (s == "off") return LOOK_FLAT;   // what a script tries first
  for (int l = 0; l < LOOK_COUNT; l++)
    if (s == lookName((uint8_t)l)) return l;
  return -1;
}

// Frames per second times ten, over the counted frames' own span.
uint32_t fpsX10() {
  if (g_run.frames < 2) return 0;
  const uint32_t span = g_run.lastMs - g_run.firstMs;
  return span ? (uint32_t)((uint64_t)(g_run.frames - 1) * 10000u / span) : 0;
}

// One parseable line: what showed, and what it cost. stack_min_free is
// loopTask's least free stack since boot, in bytes (ESP-IDF's
// uxTaskGetStackHighWaterMark counts bytes, not words), not this run's.
void report(const char *what) {
  const uint32_t n = g_run.frames ? g_run.frames : 1;
  const uint32_t fps = fpsX10();
  Serial.printf("[fx3d] scene=%s mode=%s frames=%u open_us=%u frame_us=%u frame_us_max=%u blit_us=%u blit_us_max=%u "
                "fps=%u.%u heap_internal=%u heap_internal_min=%u largest_block=%u psram_free=%u stack_min_free=%u\n",
                what, modeName(g_ctx.st.mode), (unsigned)g_run.frames, (unsigned)g_run.openUs,
                (unsigned)(g_run.renderSum / n), (unsigned)g_run.renderMax, (unsigned)(g_run.blitSum / n),
                (unsigned)g_run.blitMax, (unsigned)(fps / 10), (unsigned)(fps % 10),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)(g_run.heapMin == UINT32_MAX ? 0 : g_run.heapMin),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// The bench: every scene in mono and then in red-blue, then every look the
// same way over whatever page is up, kRunMs each. One line per run, then
// "bench done". The bench build starts it kWaitMs after boot, when the
// network has settled; any build with the flag starts it from /api/fx3d?bench=1.
const uint32_t kWaitMs = 20000, kRunMs = 5000;
const int kSceneRuns = 2 * kCatalogCount, kLookRuns = 2 * (LOOK_COUNT - 1);
enum BenchState : uint8_t { BENCH_IDLE, BENCH_ARMED, BENCH_RUNNING };
BenchState g_bench = BENCH_IDLE;
int g_benchRun = 0;
uint32_t g_armedMs = 0, g_runStartMs = 0;
char g_benchWhat[24];
Stereo g_savedProfile;          // the owner's, while the bench borrows the frame
uint8_t g_savedLook = LOOK_FLAT;

void benchStart(int run);

void benchBegin() {
  g_savedProfile = g_ctx.st;
  g_savedLook = g_look;
  g_bench = BENCH_RUNNING;
  g_benchRun = 0;
  Serial.printf("[fx3d] bench started: %d runs of %u ms (scenes, then looks); ends with \"[fx3d] bench done\"\n",
                kSceneRuns + kLookRuns, (unsigned)kRunMs);
  benchStart(0);
}

void benchEnd(const char *why) {
  closeScene();
  setLook(LOOK_FLAT);
  g_bench = BENCH_IDLE;
  g_ctx.st = g_savedProfile;
  if (g_savedLook != LOOK_FLAT) setLook(g_savedLook);
  Serial.printf("[fx3d] bench %s runs=%d\n", why, g_benchRun);
}

void benchStart(int run) {
  g_ctx.st.mode = (run % 2) ? MODE_RED_BLUE : MODE_MONO;
  bool ok;
  if (run < kSceneRuns) {
    ok = openScene(run / 2);
    snprintf(g_benchWhat, sizeof g_benchWhat, "%s", kCatalog[run / 2].id);
  } else {
    closeScene();
    const uint8_t look = (uint8_t)(1 + (run - kSceneRuns) / 2);
    ok = setLook(look);
    snprintf(g_benchWhat, sizeof g_benchWhat, "look:%s", lookName(look));
  }
  if (!ok) {
    Serial.printf("[fx3d] no PSRAM for %s\n", g_benchWhat);
    benchEnd("stopped");
    return;
  }
  g_runStartMs = millis();
}

void sendError(const char *what) {
  char buf[128];
  snprintf(buf, sizeof buf, "{\"error\":\"%s\"}", what);
  server.send(400, "application/json", buf);
}

void sendState() {
  char buf[768];
  const unsigned depth100 = (unsigned)(g_ctx.st.depthPx * 100.0f + 0.5f);
  int n = snprintf(buf, sizeof buf,
                   "{\"scene\":\"%s\",\"page\":%d,\"bench\":%s,\"look\":\"%s\",\"mode\":\"%s\",\"depthPx\":%u.%02u,"
                   "\"swap\":%s,\"gainL\":%u,\"gainR\":%u,\"frameUs\":%u,\"blitUs\":%u,\"fps\":%u.%u,\"openUs\":%u,"
                   "\"looks\":[",
                   g_sceneIdx >= 0 ? kCatalog[g_sceneIdx].id : "",
                   (g_sceneIdx >= 0 && !strcmp(kCatalog[g_sceneIdx].id, "calib")) ? static_cast<CalibScene *>(g_scene)->page : -1,
                   g_bench != BENCH_IDLE ? "true" : "false", lookName(g_look), modeName(g_ctx.st.mode),
                   depth100 / 100, depth100 % 100, g_ctx.st.swapEyes ? "true" : "false",
                   (unsigned)(g_ctx.st.gainL * 100.0f + 0.5f), (unsigned)(g_ctx.st.gainR * 100.0f + 0.5f),
                   (unsigned)(g_run.frames ? g_run.renderSum / g_run.frames : 0),
                   (unsigned)(g_run.frames ? g_run.blitSum / g_run.frames : 0), (unsigned)(fpsX10() / 10),
                   (unsigned)(fpsX10() % 10), (unsigned)g_run.openUs);
  for (int l = 0; l < LOOK_COUNT && n > 0 && n < (int)sizeof buf - 24; l++)
    n += snprintf(buf + n, sizeof buf - n, "%s\"%s\"", l ? "," : "", lookName((uint8_t)l));
  if (n > 0 && n < (int)sizeof buf - 16) n += snprintf(buf + n, sizeof buf - n, "],\"scenes\":[");
  for (int i = 0; i < kCatalogCount && n > 0 && n < (int)sizeof buf - 24; i++)
    n += snprintf(buf + n, sizeof buf - n, "%s\"%s\"", i ? "," : "", kCatalog[i].id);
  if (n > 0 && n < (int)sizeof buf - 3) {
    buf[n++] = ']';
    buf[n++] = '}';
    buf[n] = 0;
  }
  server.send(200, "application/json", buf);
}

// Strict numbers: the whole argument must be the number, inside the range.
// toFloat() and toInt() read "abc" as 0, which would black out an eye.
bool argFloat(const char *name, float lo, float hi, float &out) {
  const String a = server.arg(name);
  char *end = nullptr;
  const float v = strtof(a.c_str(), &end);
  if (!a.length() || end == a.c_str() || *end || !(v >= lo && v <= hi)) return false;
  out = v;
  return true;
}
bool argLong(const char *name, long lo, long hi, long &out) {
  const String a = server.arg(name);
  char *end = nullptr;
  const long v = strtol(a.c_str(), &end, 10);
  if (!a.length() || end == a.c_str() || *end || v < lo || v > hi) return false;
  out = v;
  return true;
}

// GET /api/fx3d                                   - what is on, every look and scene
// GET /api/fx3d?look=pop&mode=redblue&depth=2     - any page in 3D; look=flat (or off) turns it off
// GET /api/fx3d?scene=cube&mode=mono              - a scene covers the screen; scene=off gives it back
// GET /api/fx3d?scene=calib&page=3                - the calibration's six pages, 0..5
// GET /api/fx3d?bench=1 (or 0)                    - measure everything now, or stop
// Also swap=0|1, gl= and gr= 0..100 (the eyes' gains, %). Every argument is
// checked before anything changes: a bad request changes nothing. A request
// PSRAM cannot serve may already have closed the scene it replaces, and with
// both scene= and look=, the new scene can be up when the look then fails.
void handleApi() {
  if (server.hasArg("bench")) {
    const String b = server.arg("bench");
    if (b != "0" && b != "1") return sendError("bench: 0 or 1");
    if (!g_block) return sendError("no PSRAM: 3D is off");
    if (b == "1" && g_bench != BENCH_RUNNING) benchBegin();   // armed or idle: now
    else if (b == "0" && g_bench == BENCH_RUNNING) benchEnd("stopped");
    else if (b == "0") g_bench = BENCH_IDLE;                  // disarm
    return sendState();
  }
  if (g_bench == BENCH_RUNNING && server.args() > 0) {
    server.send(409, "application/json", "{\"error\":\"the bench is running; bench=0 stops it\"}");
    return;
  }
  // Every argument is read and checked first.
  int idx = -2;   // -2 not asked, -1 off
  if (server.hasArg("scene")) {
    const String sc = server.arg("scene");
    idx = sc == "off" ? -1 : catalogFind(sc.c_str());
    if (idx < 0 && sc != "off") return sendError("no such scene");
  }
  int look = -1;
  if (server.hasArg("look") && (look = lookByName(server.arg("look"))) < 0) return sendError("no such look");
  int mode = -1;
  if (server.hasArg("mode") && (mode = modeByName(server.arg("mode"))) < 0)
    return sendError("mode: mono, redblue, redcyan or redgreen");
  float depth = -1.0f;
  // A guard on the input, not a comfort limit: the brief starts at 1-2 px.
  if (server.hasArg("depth") && !argFloat("depth", 0.0f, 16.0f, depth)) return sendError("depth: 0 to 16 pixels");
  long swap = -1, gl = -1, gr = -1, page = -1;
  if (server.hasArg("swap") && !argLong("swap", 0, 1, swap)) return sendError("swap: 0 or 1");
  if (server.hasArg("gl") && !argLong("gl", 0, 100, gl)) return sendError("gl: 0 to 100");
  if (server.hasArg("gr") && !argLong("gr", 0, 100, gr)) return sendError("gr: 0 to 100");
  if (server.hasArg("page") && !argLong("page", 0, CalibScene::kPages - 1, page)) return sendError("page: 0 to 5");
  if (!g_block) return sendError("no PSRAM: 3D is off");
  // Then what can fail for memory; the profile changes only once it has not.
  // One mode for everything the owner looks at: a scene does not bring its own.
  if (idx == -1) closeScene();
  if (idx >= 0 && idx != g_sceneIdx && !openScene(idx)) return sendError("no PSRAM for the scene");
  if (look >= 0 && !setLook((uint8_t)look)) return sendError("no PSRAM for the look");
  if (mode >= 0) g_ctx.st.mode = (uint8_t)mode;
  if (depth >= 0.0f) g_ctx.st.depthPx = depth;
  if (swap >= 0) g_ctx.st.swapEyes = swap != 0;
  if (gl >= 0) g_ctx.st.gainL = (float)gl / 100.0f;
  if (gr >= 0) g_ctx.st.gainR = (float)gr / 100.0f;
  if (page >= 0 && g_sceneIdx >= 0 && !strcmp(kCatalog[g_sceneIdx].id, "calib"))
    static_cast<CalibScene *>(g_scene)->page = (int)page;
  sendState();
}

}  // namespace

void fx3dBegin() {
  const uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!allocBlock()) {
    Serial.println("[fx3d] no PSRAM for the frame buffers: 3D is off");
    return;
  }
  server.on("/api/fx3d", HTTP_GET, handleApi);
  server.on("/fx3d", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", kFx3dPage); });
  Serial.printf("[fx3d] %d scenes, %d looks, %u B of frame buffers in PSRAM, internal free %u -> %u B\n",
                kCatalogCount, LOOK_COUNT - 1, (unsigned)(kFrameBytes + sizeof(Encoder) + kW * 3), (unsigned)before,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#if defined(FX3D_BENCH)
  g_bench = BENCH_ARMED;
  g_armedMs = millis();
  Serial.printf("[fx3d] bench armed: %d runs of %u ms (scenes, then looks), starting in %u ms; "
                "ends with \"[fx3d] bench done\"\n",
                kSceneRuns + kLookRuns, (unsigned)kRunMs, (unsigned)kWaitMs);
#endif
}

void fx3dLoop() {
  if (g_bench == BENCH_IDLE || !g_block) return;
  if (g_bench == BENCH_ARMED) {
    if (millis() - g_armedMs < kWaitMs) return;
    benchBegin();
    return;
  }
  if (millis() - g_runStartMs < kRunMs) return;
  report(g_benchWhat);
  if (++g_benchRun >= kSceneRuns + kLookRuns) {
    benchEnd("done");
    return;
  }
  benchStart(g_benchRun);
}

bool fx3dOwnsScreen() { return g_scene != nullptr; }

int fx3dRefreshHz(int pageHz) {
  if (g_scene) return kSceneHz;
  if (!display.capturing()) return pageHz;
  // A look renders and blits the whole frame at every flip, so it caps the
  // rate at 30 Hz even on a page that wants 60. The looks that move need
  // frames of their own; the ones for the glasses only follow the page.
  const bool moving = g_look == LOOK_WIGGLE || g_look == LOOK_CARD || g_look == LOOK_RELIEF || g_look == LOOK_DRUM;
  const int hz = (moving && pageHz < kSceneHz) ? kSceneHz : pageHz;
  return hz > kSceneHz ? kSceneHz : hz;
}

void fx3dRender() {
  if (!g_scene) return;
  const float dt = frameDt();
  Env env;
  fillEnv(env);
  const uint32_t t0 = micros();
  renderFrame(*g_scene, g_ctx, dt, env);
  const uint32_t t1 = micros();
  blit();
  note(t1 - t0, micros() - t1);
}

void fx3dStop() {
  if (!g_scene) return;
  if (g_bench == BENCH_RUNNING) {
    benchEnd("stopped by the knob");
    return;
  }
  closeScene();
}

#endif // FX3D_ENABLED
