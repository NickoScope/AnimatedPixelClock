// Host test for src/fx3d: the projections, the rasteriser, the compositor, the
// encode, and every scene in the catalogue. No board, no Arduino.
//
// The stereo checks are the brief's section 10 (docs/drafts/27-anaglyph-
// handoff-2026-09-18.md in the knowledge base). The CIE table is compared with
// the HUB75 library's own file when a build has fetched it; its path is the
// first argument, or "-" when there is none.
//
// Built and run by tools/fx3d/check_fx3d.py, as C++11 and C++17, under the
// address and undefined-behaviour sanitizers.

#define FX3D_STATS 1

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include "fx3d_catalog.h"
#include "fx3d_present.h"
#include "synth_sound.h"

using namespace fx3d;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    g_checks++;                                                   \
    if (!(c)) {                                                   \
      g_fail++;                                                   \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);    \
    }                                                             \
  } while (0)

// Every operator new is counted: a frame must not allocate.
static long g_news = 0;
void *operator new(size_t n) {
  g_news++;
  void *p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void *operator new[](size_t n) {
  g_news++;
  void *p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
#endif

struct Bufs {
  std::vector<uint8_t> rgb, left, right;
  std::vector<float> z;
  Bufs() : rgb(kPixels * 3), left(kPixels), right(kPixels), z(kPixels) {}
  void bind(Ctx &c) {
    c.fb.rgb = rgb.data();
    c.fb.eye[0] = left.data();
    c.fb.eye[1] = right.data();
    c.fb.z = z.data();
  }
};

// ── the brief, section 10: projection ────────────────────────────────────────
static void projection() {
  Rng rng(7);
  for (int k = 0; k < 4000; k++) {
    View v;
    v.f = rng.range(40.0f, 200.0f);
    v.z0 = rng.range(2.0f, 20.0f);
    v.b = rng.range(0.0f, 0.5f);
    float xl, yl, xr, yr, xm, ym;
    // Z = Z0 gives zero parallax.
    V3 p = v3(rng.range(-3.0f, 3.0f), rng.range(-2.0f, 2.0f), v.z0);
    v.project(p, -1, xl, yl);
    v.project(p, 1, xr, yr);
    v.project(p, 0, xm, ym);
    CHECK(std::fabs(xl - xr) < 2e-3f);
    CHECK(std::fabs(xm - 0.5f * (xl + xr)) < 2e-3f);
    // Anywhere: no vertical parallax, and the disparity is f*b*(1/z - 1/z0).
    V3 q = v3(rng.range(-3.0f, 3.0f), rng.range(-2.0f, 2.0f), rng.range(0.5f, 30.0f));
    v.project(q, -1, xl, yl);
    v.project(q, 1, xr, yr);
    CHECK(yl == yr);
    const float d = v.disparity(q.z);
    CHECK(std::fabs((xl - xr) - d) < 2e-3f * (1.0f + std::fabs(d)));
    // Near and far points diverge in opposite directions.
    if (v.b > 1e-3f && q.z < 0.98f * v.z0) CHECK(xl - xr > 0.0f);
    if (v.b > 1e-3f && q.z > 1.02f * v.z0) CHECK(xl - xr < 0.0f);
  }
  // b = 0: both eyes see exactly the mono picture.
  for (int k = 0; k < 1000; k++) {
    View v;
    v.b = 0.0f;
    V3 q = v3(rng.range(-3.0f, 3.0f), rng.range(-2.0f, 2.0f), rng.range(0.5f, 30.0f));
    float xl, yl, xr, yr, xm, ym;
    v.project(q, -1, xl, yl);
    v.project(q, 1, xr, yr);
    v.project(q, 0, xm, ym);
    CHECK(xl == xr && xl == xm && yl == yr && yl == ym);
  }
}

// The baseline never lets a depth inside the scene's range exceed the budget,
// and it spends all of it at one end.
static void baselineBudget() {
  Rng rng(11);
  for (int k = 0; k < 500; k++) {
    const float f = rng.range(40.0f, 200.0f), z0 = rng.range(2.0f, 15.0f);
    const float zn = rng.range(0.6f, 14.0f), zf = zn + rng.range(0.0f, 20.0f);
    const float dmax = rng.range(0.5f, 6.0f);
    View v;
    v.f = f;
    v.z0 = z0;
    v.b = baselineFor(f, z0, zn, zf, dmax);
    float worst = 0.0f;
    for (int i = 0; i <= 200; i++) {
      float z = zn + (zf - zn) * (float)i / 200.0f;
      float d = std::fabs(v.disparity(z));
      CHECK(d <= dmax * 1.0005f + 1e-5f);
      if (d > worst) worst = d;
    }
    if (std::fabs(1.0f / zn - 1.0f / z0) > 1e-3f || std::fabs(1.0f / zf - 1.0f / z0) > 1e-3f)
      CHECK(std::fabs(worst - dmax) < 1e-3f * dmax);
  }
  CHECK(baselineFor(80.0f, 5.0f, 0.0f, 10.0f, 2.0f) == 0.0f);    // a range reaching the eye: no stereo
  CHECK(baselineFor(80.0f, 5.0f, 5.0f, 5.0f, 2.0f) == 0.0f);     // everything on the plane: nothing to spend
}

// Near-plane clipping happens before the divide: no infinity, no NaN, nothing
// drawn from behind the eye.
static void nearClip() {
  V3 a = v3(0.0f, 0.0f, -5.0f), b = v3(1.0f, 1.0f, 10.0f);
  float va = 1.0f, vb = 0.0f;
  CHECK(clipNear(a, b, va, vb, 0.5f));
  CHECK(a.z == 0.5f && std::isfinite(a.x) && std::isfinite(a.y) && va >= 0.0f && va <= 1.0f);
  V3 c = v3(0.0f, 0.0f, -1.0f), d = v3(0.0f, 0.0f, 0.2f);
  float vc = 1.0f, vd = 1.0f;
  CHECK(!clipNear(c, d, vc, vd, 0.5f));

  Bufs bufs;
  Ctx ctx;
  bufs.bind(ctx);
  ctx.view.nearZ = 0.5f;
  const float nan = std::nanf(""), inf = INFINITY;
  for (int e = -1; e <= 1; e++) {
    ctx.eye = e;
    ctx.view.b = e ? 0.2f : 0.0f;
    ctx.line(v3(0, 0, -3), v3(0.5f, 0.2f, 0.0f), 1, 1);          // all behind the eye
    ctx.line(v3(-1, 0, -3), v3(1, 0, 3), 1, 1);                  // through the plane
    ctx.line(v3(nan, 0, 3), v3(1, 0, 3), 1, 1);
    ctx.line(v3(inf, 0, 3), v3(1, 0, 3), 1, 1);
    ctx.line(v3(1e20f, 1e20f, 3), v3(-1e20f, 0, 3), 1, 1);
    ctx.point(v3(nan, nan, nan), 1);
    ctx.dot(v3(0, 0, 0.5f), 1e9f, 1);
    ctx.tri(v3(-1, -1, -2), v3(1, -1, 3), v3(0, 1, 3), 1, 1, 1, false);
    ctx.tri(v3(nan, 0, 3), v3(1, 0, 3), v3(0, 1, 3), 1, 1, 1, false);
    ctx.tri(v3(-1e6f, -1e6f, 1), v3(1e6f, -1e6f, 1), v3(0, 1e6f, 1), 1, 1, 1, false);
  }
  CHECK(ctx.stats.zMin >= 0.5f);   // nothing nearer than the plane reached a raster
}

// ── the brief, section 10: colour ────────────────────────────────────────────
static void compose() {
  std::vector<uint8_t> L(kPixels), R(kPixels), rgb(kPixels * 3);
  Rng rng(3);
  for (int i = 0; i < kPixels; i++) {
    L[i] = (uint8_t)(rng.next() & 0xff);
    R[i] = (uint8_t)(rng.next() & 0xff);
  }
  for (int mode = MODE_RED_BLUE; mode <= MODE_RED_GREEN; mode++)
    for (int swap = 0; swap <= 1; swap++) {
      Stereo st;
      st.mode = (uint8_t)mode;
      st.swapEyes = swap != 0;
      composeAnaglyph(L.data(), R.data(), rgb.data(), st);
      bool ok = true;
      for (int i = 0; i < kPixels; i++) {
        const uint8_t red = swap ? R[i] : L[i], other = swap ? L[i] : R[i];
        const uint8_t g = (mode == MODE_RED_CYAN || mode == MODE_RED_GREEN) ? other : 0;
        const uint8_t b = (mode == MODE_RED_BLUE || mode == MODE_RED_CYAN) ? other : 0;
        ok = ok && rgb[3 * i] == red && rgb[3 * i + 1] == g && rgb[3 * i + 2] == b;
      }
      CHECK(ok);   // red-blue has no green; the swap really swaps the views
    }
  // The gains scale light, and zero gain is dark.
  Stereo st;
  st.gainL = 0.5f;
  st.gainR = 0.0f;
  composeAnaglyph(L.data(), R.data(), rgb.data(), st);
  bool half = true;
  for (int i = 0; i < kPixels; i++) half = half && std::abs((int)rgb[3 * i] - L[i] / 2) <= 1 && rgb[3 * i + 2] == 0;
  CHECK(half);
}

// ── the encode: the library's CIE table, inverted ────────────────────────────
static bool readLibraryTable(const char *path, std::vector<int> &out) {
  std::ifstream f(path);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string s = ss.str();
  size_t at = s.find("lumConvTab_8bit[256] = {");
  if (at == std::string::npos) return false;
  size_t end = s.find("};", at);
  std::string body = s.substr(at + 24, end - at - 24);
  for (size_t i = 0; i < body.size();) {
    if (body[i] >= '0' && body[i] <= '9') {
      int v = 0;
      while (i < body.size() && body[i] >= '0' && body[i] <= '9') v = v * 10 + (body[i++] - '0');
      out.push_back(v);
    } else {
      i++;
    }
  }
  return out.size() == 256;
}

static void encode(const char *libPath) {
  std::vector<int> lib;
  if (libPath && std::strcmp(libPath, "-") && readLibraryTable(libPath, lib)) {
    bool same = true;
    for (int i = 0; i < 256; i++) same = same && lib[i] == kCie8[i];
    CHECK(same);
    std::printf("kCie8 compared with %s\n", libPath);
  } else {
    std::printf("kCie8 NOT compared: no library file (build the firmware once to fetch it)\n");
  }
  // The formula the library's generator prints in cie_luts.h.
  int formula = 0;
  for (int v = 0; v < 256; v++) {
    const double L = v * 100.0 / 255.0;
    const double Y = L <= 8.0 ? L / 902.3 : std::pow((L + 16.0) / 116.0, 3.0);
    if ((int)std::lround(Y * 255.0) == kCie8[v]) formula++;
  }
  CHECK(formula == 256);
  const Encoder enc;
  CHECK(enc.code[0] == 0);   // black stays black
  bool mono = true, close = true;
  for (int d = 0; d < 256; d++) {
    if (d && enc.code[d] < enc.code[d - 1]) mono = false;
    if (std::abs((int)kCie8[enc.code[d]] - d) > 1) close = false;
  }
  CHECK(mono);
  CHECK(close);   // every linear level lands within one step of 255 of itself
}

// ── the rasteriser ───────────────────────────────────────────────────────────
struct PlotAcc {
  std::vector<double> *acc;
  void operator()(int x, int y, float v) const {
    if ((unsigned)x < (unsigned)kW && (unsigned)y < (unsigned)kH && v > 0.0f) (*acc)[y * kW + x] += v;
  }
};

static void lines() {
  Rng rng(5);
  std::vector<double> acc(kPixels);
  PlotAcc p = {&acc};
  for (int k = 0; k < 2000; k++) {
    std::fill(acc.begin(), acc.end(), 0.0);
    const float x0 = rng.range(4.0f, kW - 5.0f), y0 = rng.range(4.0f, kH - 5.0f);
    const float x1 = rng.range(4.0f, kW - 5.0f), y1 = rng.range(4.0f, kH - 5.0f);
    line(p, x0, y0, x1, y1, 1.0f, 1.0f);
    double sum = 0, cx = 0, cy = 0;
    for (int i = 0; i < kPixels; i++) {
      sum += acc[i];
      cx += acc[i] * (i % kW);
      cy += acc[i] * (i / kW);
    }
    const double len = std::sqrt((double)(x1 - x0) * (x1 - x0) + (double)(y1 - y0) * (y1 - y0));
    if (len < 0.05) continue;
    CHECK(std::fabs(sum - len) < 1e-3 * len + 1e-4);     // the same light per unit length at any angle
    // Centred where the line is. Across the line exactly: that is what makes a
    // sub-pixel disparity visible. Along it, an end column covered by a
    // fraction c holds its light at the pixel centre, (1 - c)/2 from where it
    // belongs: c(1 - c)/2 <= 1/8, with opposite signs at the two ends, so the
    // centroid moves by at most 1/8 of a pixel over the length along the major
    // axis.
    const bool steep = std::fabs(y1 - y0) > std::fabs(x1 - x0);
    const double major = steep ? std::fabs(y1 - y0) : std::fabs(x1 - x0);
    const double ex = std::fabs(cx / sum - 0.5 * ((double)x0 + x1)), ey = std::fabs(cy / sum - 0.5 * ((double)y0 + y1));
    const double along = 0.125 / (major > 1.0 ? major : 1.0) + 1e-4;
    CHECK((steep ? ey : ex) <= along);
    CHECK((steep ? ex : ey) < 1e-3);
  }
}

static void splats() {
  Rng rng(9);
  std::vector<double> acc(kPixels);
  PlotAcc p = {&acc};
  for (int k = 0; k < 2000; k++) {
    std::fill(acc.begin(), acc.end(), 0.0);
    const float x = rng.range(1.0f, kW - 2.0f), y = rng.range(1.0f, kH - 2.0f);
    splat(p, x, y, 1.0f);
    double sum = 0, cx = 0, cy = 0;
    for (int i = 0; i < kPixels; i++) {
      sum += acc[i];
      cx += acc[i] * (i % kW);
      cy += acc[i] * (i / kW);
    }
    CHECK(std::fabs(sum - 1.0) < 1e-5);
    CHECK(std::fabs(cx / sum - x) < 1e-4 && std::fabs(cy / sum - y) < 1e-4);   // sub-pixel position kept
  }
}

struct FragCount {
  std::vector<int> *n;
  void operator()(int i, float) const { (*n)[i]++; }
};

static float odd16(float v) { return (float)(((int)(v * 16.0f)) | 1) / 16.0f; }

// A jittered mesh over a rectangle: every pixel centre inside is covered by
// exactly one triangle, and none outside.
static void triangleCoverage() {
  Rng rng(13);
  std::vector<int> n(kPixels);
  std::vector<float> zb(kPixels);
  FragCount frag = {&n};
  for (int trial = 0; trial < 40; trial++) {
    std::fill(n.begin(), n.end(), 0);
    const int nx = 3 + (int)(rng.next() % 9), ny = 2 + (int)(rng.next() % 6);
    // The outline on odd sixteenths: exactly where the rasteriser snaps it, and
    // never on a pixel centre, so "inside" has one meaning for both.
    const float x0 = odd16(rng.range(2.2f, 20.7f)), x1 = odd16(rng.range(100.3f, 125.4f));
    const float y0 = odd16(rng.range(1.3f, 12.6f)), y1 = odd16(rng.range(50.2f, 62.4f));
    std::vector<float> vx((nx + 1) * (ny + 1)), vy((nx + 1) * (ny + 1));
    for (int j = 0; j <= ny; j++)
      for (int i = 0; i <= nx; i++) {
        float x = x0 + (x1 - x0) * i / nx, y = y0 + (y1 - y0) * j / ny;
        if (i > 0 && i < nx) x += rng.range(-1.5f, 1.5f);
        if (j > 0 && j < ny) y += rng.range(-1.5f, 1.5f);
        vx[j * (nx + 1) + i] = x;
        vy[j * (nx + 1) + i] = y;
      }
    for (int j = 0; j < ny; j++)
      for (int i = 0; i < nx; i++) {
        const int a = j * (nx + 1) + i, b = a + 1, c = a + nx + 1, d = c + 1;
        SV A = {vx[a], vy[a], 1, 1}, B = {vx[b], vy[b], 1, 1}, C = {vx[c], vy[c], 1, 1}, D = {vx[d], vy[d], 1, 1};
        std::fill(zb.begin(), zb.end(), 0.0f);
        if (rng.next() & 1) {
          triangle(A, B, D, zb.data(), frag, false);
          std::fill(zb.begin(), zb.end(), 0.0f);
          triangle(A, D, C, zb.data(), frag, false);
        } else {
          triangle(A, B, C, zb.data(), frag, false);
          std::fill(zb.begin(), zb.end(), 0.0f);
          triangle(B, D, C, zb.data(), frag, false);
        }
      }
    bool once = true, none = true;
    for (int y = 0; y < kH; y++)
      for (int x = 0; x < kW; x++) {
        const bool in = x > x0 && x < x1 && y > y0 && y < y1;
        if (in && n[y * kW + x] != 1) once = false;
        if (!in && n[y * kW + x] != 0) none = false;
      }
    CHECK(once);   // no holes, no pixel drawn twice along a shared edge
    CHECK(none);
  }
}

struct FragShade {
  std::vector<float> *s;
  void operator()(int i, float v) const { (*s)[i] = v; }
};

static void triangleDepthAndCull() {
  std::vector<float> zb(kPixels), sh(kPixels);
  FragShade f = {&sh};
  // Counter-clockwise with y up (y grows down on screen): front-facing.
  SV a = {20, 40, 0.5f, 0.2f}, b = {60, 40, 0.5f, 0.2f}, c = {20, 10, 0.5f, 0.2f};
  std::fill(zb.begin(), zb.end(), 0.0f);
  std::fill(sh.begin(), sh.end(), -1.0f);
  triangle(a, b, c, zb.data(), f, true);
  CHECK(sh[30 * kW + 25] == 0.2f);
  std::fill(sh.begin(), sh.end(), -1.0f);
  std::fill(zb.begin(), zb.end(), 0.0f);
  triangle(a, c, b, zb.data(), f, true);   // the same, clockwise: culled
  CHECK(sh[30 * kW + 25] == -1.0f);
  // The nearer of two overlapping triangles wins, whatever the order.
  SV n1 = {10, 50, 0.5f, 0.9f}, n2 = {80, 50, 0.5f, 0.9f}, n3 = {10, 5, 0.5f, 0.9f};
  SV f1 = {10, 50, 0.25f, 0.1f}, f2 = {80, 50, 0.25f, 0.1f}, f3 = {10, 5, 0.25f, 0.1f};
  for (int order = 0; order < 2; order++) {
    std::fill(zb.begin(), zb.end(), 0.0f);
    std::fill(sh.begin(), sh.end(), -1.0f);
    if (order) {
      triangle(n1, n2, n3, zb.data(), f, false);
      triangle(f1, f2, f3, zb.data(), f, false);
    } else {
      triangle(f1, f2, f3, zb.data(), f, false);
      triangle(n1, n2, n3, zb.data(), f, false);
    }
    CHECK(sh[40 * kW + 20] == 0.9f);
  }
}

// ── every scene in the catalogue ─────────────────────────────────────────────
static bool anyLight(const std::vector<uint8_t> &rgb) {
  for (size_t i = 0; i < rgb.size(); i++)
    if (rgb[i]) return true;
  return false;
}

static void scenes() {
  for (int k = 0; k < kCatalogCount; k++) {
    const CatalogEntry &e = kCatalog[k];
    for (int mode = MODE_MONO; mode <= MODE_RED_CYAN; mode++) {
      std::vector<unsigned char> mem(e.bytes), mem2(e.bytes);
      Bufs b1, b2;
      Ctx c1, c2;
      b1.bind(c1);
      b2.bind(c2);
      c1.st.mode = c2.st.mode = (uint8_t)mode;
      Scene *s1 = e.make(mem.data());
      Scene *s2 = e.make(mem2.data());
      s1->reset(42);
      s2->reset(42);
      Env w;
      w.valid = true;
      w.hour = 23;
      w.minute = 59;
      w.second = 58;
      const long news = g_news;
      bool light = false, noGreen = true, inRange = true;
      float zn = 0, zf = 0;
      for (int f = 0; f < 90; f++) {
        synthSound((float)f / 30.0f, w);
        c1.stats.clear();
        renderFrame(*s1, c1, 1.0f / 30.0f, w);
        renderFrame(*s2, c2, 1.0f / 30.0f, w);
        light = light || anyLight(b1.rgb);
        s1->depthRange(zn, zf);
        if (mode != MODE_MONO && !s1->raw()) {
          for (int i = 0; i < kPixels; i++) noGreen = noGreen && (mode != MODE_RED_BLUE || b1.rgb[3 * i + 1] == 0);
          if (c1.stats.zMin <= c1.stats.zMax)
            inRange = inRange && c1.stats.zMin >= zn - 1e-3f && c1.stats.zMax <= zf + 1e-3f;
        }
      }
      CHECK(g_news == news);            // no allocation in a frame
      CHECK(light);                     // it draws something
      CHECK(noGreen);                   // red-blue carries no green
      CHECK(inRange);                   // nothing outside the depth the baseline was sized for
      CHECK(b1.rgb == b2.rgb);          // the same seed gives the same frames
      if (!light || !inRange) std::printf("  scene %s mode %d\n", e.id, mode);
      // A month of uptime in one-hour steps: phases stay wrapped and it still draws.
      for (int h = 0; h < 24 * 31; h++) s1->step(3600.0f, w);
      renderFrame(*s1, c1, 1.0f / 30.0f, w);
      CHECK(anyLight(b1.rgb));
      s1->~Scene();
      s2->~Scene();
    }
  }
}


// ── 3D as a look for any screen ──────────────────────────────────────────────
// A made-up page: black, a white bar, a dim red block, a bright blue one, and
// a sweep of greys, as codes.
static std::vector<uint8_t> testPage() {
  std::vector<uint8_t> p(kPixels * 3, 0);
  for (int y = 10; y < 20; y++)
    for (int x = 10; x < 60; x++) p[3 * (y * kW + x)] = p[3 * (y * kW + x) + 1] = p[3 * (y * kW + x) + 2] = 255;
  for (int y = 30; y < 50; y++)
    for (int x = 20; x < 40; x++) p[3 * (y * kW + x)] = 90;
  for (int y = 30; y < 50; y++)
    for (int x = 70; x < 100; x++) p[3 * (y * kW + x) + 2] = 255;
  for (int x = 0; x < kW; x++) p[3 * (60 * kW + x)] = p[3 * (60 * kW + x) + 1] = p[3 * (60 * kW + x) + 2] = (uint8_t)(2 * x);
  return p;
}

static double centroidX(const std::vector<uint8_t> &plane) {
  double s = 0, sx = 0;
  for (int i = 0; i < kPixels; i++) {
    s += plane[i];
    sx += (double)plane[i] * (i % kW);
  }
  return s > 0 ? sx / s : -1;
}

static void looks() {
  const std::vector<uint8_t> page = testPage();
  for (int look = 0; look < LOOK_COUNT; look++)
    for (int mode = MODE_MONO; mode <= MODE_RED_CYAN; mode++) {
      Bufs b1, b2;
      Ctx c1, c2;
      b1.bind(c1);
      b2.bind(c2);
      c1.st.mode = c2.st.mode = (uint8_t)mode;
      std::vector<unsigned char> m1(sizeof(PictureScene)), m2(sizeof(PictureScene));
      PictureScene *s1 = new (m1.data()) PictureScene(), *s2 = new (m2.data()) PictureScene();
      s1->look = s2->look = (uint8_t)look;
      s1->codes = s2->codes = page.data();
      s1->reset(1);
      s2->reset(1);
      Env w;
      const long news = g_news;
      bool light = false, inRange = true;
      for (int f = 0; f < 60; f++) {
        c1.stats.clear();
        renderFrame(*s1, c1, 1.0f / 30.0f, w);
        renderFrame(*s2, c2, 1.0f / 30.0f, w);
        light = light || anyLight(b1.rgb);
        float zn, zf;
        s1->depthRange(zn, zf);
        if (mode != MODE_MONO && c1.stats.zMin <= c1.stats.zMax)
          inRange = inRange && c1.stats.zMin >= zn - 1e-3f && c1.stats.zMax <= zf + 1e-3f;
      }
      CHECK(g_news == news);
      CHECK(light);
      CHECK(inRange);
      CHECK(b1.rgb == b2.rgb);
      if (!light || !inRange) std::printf("  look %s mode %d\n", lookName((uint8_t)look), mode);
      s1->~PictureScene();
      s2->~PictureScene();
    }

  // Flat, mono: the page's own light, exactly (codes -> linear, the library's table).
  {
    Bufs b;
    Ctx c;
    b.bind(c);
    c.st.mode = MODE_MONO;
    PictureScene s;
    s.codes = page.data();
    s.look = LOOK_FLAT;
    Env w;
    renderFrame(s, c, 0.0f, w);
    bool same = true;
    for (int i = 0; i < kPixels * 3; i++) same = same && b.rgb[i] == kCie8[page[i]];
    CHECK(same);
  }
  // Float: one bright pixel moves by +d/2 for the left eye and -d/2 for the
  // right, to a hundredth of a pixel, for a fractional d.
  {
    std::vector<uint8_t> dot(kPixels * 3, 0);
    const int x0 = 60, y0 = 30;
    for (int k = 0; k < 3; k++) dot[3 * (y0 * kW + x0) + k] = 200;
    for (int trial = 0; trial < 5; trial++) {
      const float d = 0.7f + 0.9f * (float)trial;
      Bufs b;
      Ctx c;
      b.bind(c);
      c.st.mode = MODE_RED_BLUE;
      c.st.depthPx = d;
      PictureScene s;
      s.codes = dot.data();
      s.look = LOOK_FLOAT;
      Env w;
      renderFrame(s, c, 0.0f, w);
      CHECK(std::fabs(centroidX(b.left) - (x0 + 0.5 * d)) < 0.01);
      CHECK(std::fabs(centroidX(b.right) - (x0 - 0.5 * d)) < 0.01);
    }
  }
  // No budget: every look for the glasses shows both eyes the page as it is.
  for (int look = LOOK_POP; look <= LOOK_DOME; look++) {
    Bufs b;
    Ctx c;
    b.bind(c);
    c.st.mode = MODE_RED_BLUE;
    c.st.depthPx = 0.0f;
    PictureScene s;
    s.codes = page.data();
    s.look = (uint8_t)look;
    Env w;
    renderFrame(s, c, 0.0f, w);
    bool same = b.left == b.right;
    for (int i = 0; i < kPixels && same; i++) {
      const uint8_t *p = &page[3 * i];
      uint8_t m = kCie8[p[0]];
      if (kCie8[p[1]] > m) m = kCie8[p[1]];
      if (kCie8[p[2]] > m) m = kCie8[p[2]];
      same = std::abs((int)b.left[i] - (int)m) <= 1;
    }
    CHECK(same);
  }
}

int main(int argc, char **argv) {
  projection();
  baselineBudget();
  nearClip();
  compose();
  encode(argc > 1 ? argv[1] : "-");
  lines();
  splats();
  triangleCoverage();
  triangleDepthAndCull();
  scenes();
  looks();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
