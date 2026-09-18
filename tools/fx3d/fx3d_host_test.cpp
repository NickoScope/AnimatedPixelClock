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
#include "fx3d_profile.h"
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


// ── no jump however long it runs ─────────────────────────────────────────────
// Where a wrapped time used to wrap (blobs 600 s, rings 875 s, the voxel clock
// and the looks 3600 s), the change from one frame to the next must be like the
// change at any other moment. The yardstick is the scene's own motion over five
// seconds of ordinary running, not a number from outside; a jump of the kind
// that was there (the card turning 20 degrees in one frame) is many times it.
static long frameDiff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
  long d = 0;
  for (size_t i = 0; i < a.size(); i++) d += std::abs((int)a[i] - (int)b[i]);
  return d;
}
static long worstStep(Scene &s, Ctx &c, Bufs &b, int frames) {
  Env w;
  std::vector<uint8_t> prev;
  long worst = 0;
  for (int f = 0; f < frames; f++) {
    renderFrame(s, c, 1.0f / 30.0f, w);
    if (!prev.empty()) {
      const long d = frameDiff(prev, b.rgb);
      if (d > worst) worst = d;
    }
    prev = b.rgb;
  }
  return worst;
}
static void continuity() {
  struct Case {
    const char *id;   // a scene id, or a look's name after "look:"
    float wrapAt;
  };
  const Case cases[] = {{"blobs", 600.0f}, {"rings", 875.0f}, {"vclock", 3600.0f}, {"look:card", 3600.0f},
                        {"look:relief", 3600.0f}, {"look:drum", 3600.0f}, {"look:wiggle", 3600.0f}};
  const std::vector<uint8_t> page = testPage();
  for (const Case &k : cases) {
    Bufs b;
    Ctx c;
    b.bind(c);
    c.st.mode = MODE_MONO;
    std::vector<unsigned char> mem;
    Scene *s = nullptr;
    PictureScene *pic = nullptr;
    if (!std::strncmp(k.id, "look:", 5)) {
      mem.resize(sizeof(PictureScene));
      pic = new (mem.data()) PictureScene();
      for (int l = 0; l < LOOK_COUNT; l++)
        if (!std::strcmp(k.id + 5, lookName((uint8_t)l))) pic->look = (uint8_t)l;
      pic->codes = page.data();
      s = pic;
    } else {
      const int idx = catalogFind(k.id);
      mem.resize(kCatalog[idx].bytes);
      s = kCatalog[idx].make(mem.data());
    }
    s->reset(3);
    Env w;
    for (int t = 0; t < 10; t++) s->step(1.0f, w);
    const long normal = worstStep(*s, c, b, 150);
    for (float t = 15.0f; t < k.wrapAt - 1.0f; t += 1.0f) s->step(1.0f, w);
    const long atWrap = worstStep(*s, c, b, 60);   // two seconds across the old wrap
    CHECK(atWrap <= 2 * normal + kPixels / 4);
    if (atWrap > 2 * normal + kPixels / 4) std::printf("  %s: %ld across %.0f s, %ld normally\n", k.id, atWrap, k.wrapAt, normal);
    s->~Scene();
  }
  // The check can see a jump: the card at its widest swing (1.75 s: sin = 1,
  // 20 degrees) snapped back to square in one frame must fail it.
  {
    Bufs b;
    Ctx c;
    b.bind(c);
    c.st.mode = MODE_MONO;
    PictureScene pic;
    pic.look = LOOK_CARD;
    pic.codes = page.data();
    pic.reset(3);
    Env w;
    const long normal = worstStep(pic, c, b, 150);
    pic.reset(3);
    for (int f = 0; f < 52; f++) pic.step(1.0f / 30.0f, w);   // to 1.75 s
    renderFrame(pic, c, 1.0f / 30.0f, w);
    const std::vector<uint8_t> before = b.rgb;
    pic.reset(3);
    renderFrame(pic, c, 0.0f, w);
    CHECK(frameDiff(before, b.rgb) > 2 * normal + kPixels / 4);
  }
}


// ── the blit's runs, and the landscape's cached lattices ─────────────────────
static void runs() {
  Rng rng(21);
  std::vector<uint8_t> row(kW * 3), back(kW * 3);
  for (int trial = 0; trial < 2000; trial++) {
    // Rows with long runs, short runs and single pixels.
    for (int x = 0; x < kW;) {
      const int n = 1 + (int)(rng.next() % (trial % 3 == 0 ? 40 : 4));
      const uint8_t r = (uint8_t)(rng.next() % 3 * 100), g = (uint8_t)(rng.next() % 2 * 200), b = (uint8_t)(rng.next() % 2);
      for (int k = 0; k < n && x < kW; k++, x++) {
        row[3 * x] = r;
        row[3 * x + 1] = g;
        row[3 * x + 2] = b;
      }
    }
    std::fill(back.begin(), back.end(), 7);
    int next = 0;
    bool tiles = true, maximal = true;
    const uint8_t *prev = nullptr;
    forEachRun(row.data(), [&](int x, int n, const uint8_t *c) {
      tiles = tiles && x == next && n >= 1;
      if (prev && prev[0] == c[0] && prev[1] == c[1] && prev[2] == c[2]) maximal = false;
      for (int k = 0; k < n; k++)
        for (int ch = 0; ch < 3; ch++) back[3 * (x + k) + ch] = c[ch];
      next = x + n;
      prev = c;
    });
    CHECK(tiles && next == kW);   // every pixel once, left to right
    CHECK(maximal);               // no two neighbouring runs of one colour
    CHECK(back == row);           // the runs give back the row exactly
  }
}

// The old way: every corner hashed per texel. The cached lattices must give the
// same landscape, bit for bit.
static float oldNoise(int x, int y, uint32_t seed) {
  float sum = 0.0f, amp = 0.5f, norm = 0.0f;
  for (int o = 0; o < 6; o++) {
    const int cell = 64 >> o, per = 256 / cell;
    const int gx = x / cell, gy = y / cell;
    const float fx = (float)(x % cell) / cell, fy = (float)(y % cell) / cell;
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = (float)(hash3(gx % per, gy % per, seed + o) & 0xFFFF) / 65535.0f;
    const float b = (float)(hash3((gx + 1) % per, gy % per, seed + o) & 0xFFFF) / 65535.0f;
    const float c = (float)(hash3(gx % per, (gy + 1) % per, seed + o) & 0xFFFF) / 65535.0f;
    const float d = (float)(hash3((gx + 1) % per, (gy + 1) % per, seed + o) & 0xFFFF) / 65535.0f;
    sum += amp * mixf(mixf(a, b, sx), mixf(c, d, sx), sy);
    norm += amp;
    amp *= 0.5f;
  }
  return sum / norm;
}
static void landscape() {
  const int idx = catalogFind("voxel");
  std::vector<unsigned char> mem(kCatalog[idx].bytes);
  VoxelScene *v = static_cast<VoxelScene *>(kCatalog[idx].make(mem.data()));
  const uint32_t seed = 20260918u;
  v->reset(seed);
  bool same = true;
  for (int y = 0; y < 256 && same; y++)
    for (int x = 0; x < 256 && same; x++) {
      float n = oldNoise(x, y, seed);
      n = clampf((n - 0.28f) / 0.5f, 0.0f, 1.0f);
      const float h = n * n * 150.0f + 12.0f;
      const uint8_t want = (uint8_t)(h < 34.0f ? 34.0f : clampf(h, 0.0f, 255.0f));
      same = v->mapHeight(x, y) == want;
    }
  CHECK(same);
  v->~VoxelScene();
}


// ── the faster reprojection gives exactly what the plain walk gave ───────────
// The walk as it was: every band over the whole row. The sorted version must
// match it byte for byte, for every glasses look in both eyes and for wiggle.
static void oldReproject(const PictureScene &pic, const uint8_t *codes, float shift, bool stereo, uint8_t *out) {
  const int kLevels = 8;
  std::vector<uint8_t> lin(kPixels * 3);
  for (int i = 0; i < kPixels * 3; i++) lin[i] = kCie8[codes[i]];
  for (int y = 0; y < kH; y++) {
    float band[kW][4], acc[kW][3], d[kW];
    std::memset(acc, 0, sizeof acc);
    for (int x = 0; x < kW; x++) d[x] = pic.depthOf(x, y, codes + 3 * (y * kW + x));
    for (int k = 0; k < kLevels; k++) {
      const float lo = (float)k / kLevels, hi = (float)(k + 1) / kLevels;
      bool any = false;
      std::memset(band, 0, sizeof band);
      for (int x = 0; x < kW; x++) {
        const float dd = d[x];
        if (!(k == 0 ? dd <= hi : (dd > lo && dd <= hi))) continue;
        const uint8_t *q = codes + 3 * (y * kW + x);
        if (!q[0] && !q[1] && !q[2]) continue;
        const uint8_t *p = &lin[3 * (y * kW + x)];
        const float xs = (float)x - shift * dd;
        const float fl = std::floor(xs);
        const int i0 = (int)fl;
        const float fr = xs - fl;
        const float col[3] = {p[0] * (1.0f / 255.0f), p[1] * (1.0f / 255.0f), p[2] * (1.0f / 255.0f)};
        for (int j = 0; j < 2; j++) {
          const int xi = i0 + j;
          if (xi < 0 || xi >= kW) continue;
          const float w = j ? fr : 1.0f - fr;
          band[xi][0] += col[0] * w;
          band[xi][1] += col[1] * w;
          band[xi][2] += col[2] * w;
          band[xi][3] += w;
        }
        any = true;
      }
      if (!any) continue;
      for (int x = 0; x < kW; x++) {
        const float a = band[x][3] > 1.0f ? 1.0f : band[x][3];
        if (a <= 0.0f) continue;
        const float norm = band[x][3] > 1.0f ? 1.0f / band[x][3] : 1.0f;
        for (int ch = 0; ch < 3; ch++) acc[x][ch] = acc[x][ch] * (1.0f - a) + band[x][ch] * norm;
      }
    }
    for (int x = 0; x < kW; x++) {   // Ctx::pixel's arithmetic
      const int i = y * kW + x;
      if (stereo) {
        const float m = acc[x][0] > acc[x][1] ? (acc[x][0] > acc[x][2] ? acc[x][0] : acc[x][2]) : (acc[x][1] > acc[x][2] ? acc[x][1] : acc[x][2]);
        out[i] = (uint8_t)(clampf(m, 0.0f, 1.0f) * 255.0f + 0.5f);
      } else {
        for (int ch = 0; ch < 3; ch++) out[3 * i + ch] = (uint8_t)(clampf(acc[x][ch], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
  }
}

static void reprojectMatches() {
  Rng rng(33);
  std::vector<std::vector<uint8_t>> pages;
  pages.push_back(testPage());
  for (int k = 0; k < 3; k++) {   // noisy pages with black gaps
    std::vector<uint8_t> pg(kPixels * 3);
    for (int i = 0; i < kPixels; i++) {
      const bool dark = rng.next() % 3 == 0;
      for (int ch = 0; ch < 3; ch++) pg[3 * i + ch] = dark ? 0 : (uint8_t)(rng.next() & 0xff);
    }
    pages.push_back(pg);
  }
  const float depths[] = {2.0f, 3.7f};
  for (const std::vector<uint8_t> &pg : pages)
    for (float depth : depths) {
      for (int look = LOOK_POP; look <= LOOK_DOME; look++) {
        Bufs b;
        Ctx c;
        b.bind(c);
        c.st.mode = MODE_RED_BLUE;
        c.st.depthPx = depth;
        PictureScene pic;
        pic.codes = pg.data();
        pic.look = (uint8_t)look;
        Env w;
        renderFrame(pic, c, 0.0f, w);
        std::vector<uint8_t> l(kPixels), r(kPixels);
        oldReproject(pic, pg.data(), 0.5f * -1.0f * depth, true, l.data());
        oldReproject(pic, pg.data(), 0.5f * 1.0f * depth, true, r.data());
        CHECK(b.left == l && b.right == r);
      }
      // Wiggle, mono: the eye swung by sin of its phase after one 0.1 s step.
      Bufs b;
      Ctx c;
      b.bind(c);
      c.st.mode = MODE_MONO;
      c.st.depthPx = depth;
      PictureScene pic;
      pic.codes = pg.data();
      pic.look = LOOK_WIGGLE;
      pic.reset(1);
      Env w;
      renderFrame(pic, c, 0.1f, w);
      std::vector<uint8_t> want(kPixels * 3);
      const float ph = wrapAngle(0.0f + kTwoPi * 0.1f * 2.0f);
      oldReproject(pic, pg.data(), 0.5f * depth * std::sin(ph), false, want.data());
      CHECK(b.rgb == want);
    }
}

// A stand-in for NVS behind Preferences: typed entries, gets that give the
// default for a missing key or another type, puts that can be made to fail.
// A put under a name held with another type either replaces that entry, or
// (legacy, ESP-IDF 4.4.7 - fx3d_profile.h says where) adds one more entry
// behind it, every time, and reads keep stopping at the old one until it is
// erased.
struct FakeNvs {
  enum Type { T_U8, T_U16, T_I32 };
  struct Entry {
    std::string key;
    Type type;
    uint32_t value;
  };
  std::vector<Entry> e;   // reads take the first entry of a name
  bool legacy = false, clearFails = false;
  int putsLeft = 1000, puts = 0, clears = 0;
  std::vector<std::string> order;

  const Entry *first(const char *k) const {
    for (const Entry &x : e)
      if (x.key == k) return &x;
    return nullptr;
  }
  uint8_t getUChar(const char *k, uint8_t def) const {
    const Entry *x = first(k);
    return (x && x->type == T_U8) ? (uint8_t)x->value : def;
  }
  uint16_t getUShort(const char *k, uint16_t def) const {
    const Entry *x = first(k);
    return (x && x->type == T_U16) ? (uint16_t)x->value : def;
  }
  bool isKey(const char *k) const { return first(k) != nullptr; }
  bool put(const char *k, Type t, uint32_t v) {
    if (putsLeft <= 0) return false;
    putsLeft--;
    puts++;
    order.push_back(k);
    const Entry *head = first(k);
    if (!(legacy && head && head->type != t))
      for (Entry &x : e)
        if (x.key == k && x.type == t) {
          x.value = v;
          return true;
        }
    if (!legacy)
      for (size_t i = e.size(); i-- > 0;)
        if (e[i].key == k) e.erase(e.begin() + (long)i);
    Entry n;
    n.key = k;
    n.type = t;
    n.value = v;
    e.push_back(n);
    return true;
  }
  size_t putUChar(const char *k, uint8_t v) { return put(k, T_U8, v) ? 1 : 0; }     // Preferences returns the bytes
  size_t putUShort(const char *k, uint16_t v) { return put(k, T_U16, v) ? 2 : 0; }
  bool clear() {
    if (clearFails) return false;
    clears++;
    e.clear();
    return true;
  }
};

static bool sameStereo(const Stereo &a, const Stereo &b) {
  return a.mode == b.mode && a.swapEyes == b.swapEyes && a.depthPx == b.depthPx && a.gainL == b.gainL &&
         a.gainR == b.gainR;
}

// A whole record of this layout, with one key left out, out of its range, or
// of another type (kind 0, 1, 2; -1 keeps every key).
static void putRecord(FakeNvs &n, int skip, int kind) {
  const uint32_t good[PK_COUNT] = {MODE_RED_GREEN, 1, 350, 80, 60, kProfileVersion};
  const uint32_t bad[PK_COUNT] = {MODE_COUNT, 2, kDepthMax100 + 1u, 101, 200, 2};
  for (int k = 0; k < PK_COUNT; k++) {
    const char *name = profileKeyName(k);
    const bool odd = k == skip;
    if (odd && kind == 0) continue;
    if (odd && kind == 2) {
      n.put(name, FakeNvs::T_I32, 7);
      continue;
    }
    const uint32_t v = odd ? bad[k] : good[k];
    if (profileKeyWide(k)) n.putUShort(name, (uint16_t)v);
    else n.putUChar(name, (uint8_t)v);
  }
  n.puts = 0;
  n.order.clear();
}

static void profile() {
  const Stereo def;

  // Every value the API can set comes back the same after a reboot. The page
  // and the API give gains in whole per cent and depths in 1/100 px, so the
  // round trip is exact, float for float.
  for (int m = 0; m < MODE_COUNT; m++)
    for (int sw = 0; sw < 2; sw++) {
      Stereo st;
      st.mode = (uint8_t)m;
      st.swapEyes = sw == 1;
      CHECK(sameStereo(profileFrom(profileTo(st)), st));
    }
  for (int d = 0; d <= kDepthMax100; d++) {
    Stereo st;
    st.depthPx = (float)d / 100.0f;
    CHECK(profileTo(st).v[PK_DEPTH] == d && profileFrom(profileTo(st)).depthPx == st.depthPx);
  }
  for (int g = 0; g <= 100; g++) {
    Stereo st;
    st.gainL = (float)g / 100.0f;
    st.gainR = (float)(100 - g) / 100.0f;
    const StoredProfile p = profileTo(st);
    CHECK(p.v[PK_GL] == g && p.v[PK_GR] == 100 - g && sameStereo(profileFrom(p), st));
  }
  {
    Stereo st;   // what cannot be stored is clamped, and NaN is not cast
    st.mode = 17;
    st.depthPx = 99.0f;
    st.gainL = 7.0f;
    st.gainR = -1.0f;
    const StoredProfile p = profileTo(st);
    CHECK(p.v[PK_MODE] == MODE_RED_BLUE && p.v[PK_DEPTH] == kDepthMax100 && p.v[PK_GL] == 100 && p.v[PK_GR] == 0);
    st.depthPx = -3.0f;
    st.gainL = NAN;
    CHECK(profileTo(st).v[PK_DEPTH] == 0 && profileTo(st).v[PK_GL] == 0);
    CHECK(profileTo(def).v[PK_VER] == kProfileVersion);
  }

  // Reading. Nothing stored: the defaults.
  {
    FakeNvs n;
    const ProfileStore s = profileRead(n);
    CHECK(profileStoreEmpty(s) && sameStereo(profileFrom(s.rec), def) && profileUsed(s.rec) == 0);
  }
  // A whole record: every value.
  {
    FakeNvs n;
    putRecord(n, -1, 0);
    const ProfileStore s = profileRead(n);
    const Stereo st = profileFrom(s.rec);
    CHECK(s.clean && profileUsed(s.rec) == 5);
    CHECK(st.mode == MODE_RED_GREEN && st.swapEyes && st.depthPx == 3.5f && st.gainL == 0.8f && st.gainR == 0.6f);
  }
  // No `ver`, another one, or one of another type: every default.
  for (int kind = 0; kind < 3; kind++) {
    FakeNvs n;
    putRecord(n, PK_VER, kind);
    const ProfileStore s = profileRead(n);
    CHECK(sameStereo(profileFrom(s.rec), def) && profileUsed(s.rec) == 0);
    CHECK(s.clean == (kind != 2));
  }
  // One value missing, out of its range, or of another type: that value's
  // default, the rest from the record.
  for (int k = 0; k < PK_VER; k++)
    for (int kind = 0; kind < 3; kind++) {
      FakeNvs whole, n;
      putRecord(whole, -1, 0);
      putRecord(n, k, kind);
      const ProfileStore s = profileRead(n);
      const Stereo got = profileFrom(s.rec), all = profileFrom(profileRead(whole).rec);
      Stereo want = all;
      if (k == PK_MODE) want.mode = def.mode;
      if (k == PK_SWAP) want.swapEyes = def.swapEyes;
      if (k == PK_DEPTH) want.depthPx = def.depthPx;
      if (k == PK_GL) want.gainL = def.gainL;
      if (k == PK_GR) want.gainR = def.gainR;
      CHECK(sameStereo(got, want) && profileUsed(s.rec) == 4);
      CHECK(s.clean == (kind != 2));
    }

  // Writing. The first write puts every key, `ver` last, and reads back.
  {
    FakeNvs n;
    ProfileStore s = profileRead(n);
    Stereo st;
    st.mode = MODE_RED_CYAN;
    st.depthPx = 1.5f;
    st.gainR = 0.7f;
    CHECK(profileWrite(n, s, profileTo(st)) == PK_COUNT && n.clears == 0 && n.order.back() == "ver");
    const ProfileStore back = profileRead(n);
    CHECK(back.clean && sameRecord(back.rec, s.rec) && sameStereo(profileFrom(back.rec), st));
    n.puts = 0;   // the same again: nothing written
    CHECK(profileWrite(n, s, profileTo(st)) == 0 && n.puts == 0);
    st.depthPx = 2.5f;   // one value: one key
    CHECK(profileWrite(n, s, profileTo(st)) == 1 && n.puts == 1 && n.order.back() == "depth");
    CHECK(sameStereo(profileFrom(profileRead(n).rec), st));
  }
  // A record of another layout in our types: the keys that already agree stay.
  {
    FakeNvs n;
    putRecord(n, PK_VER, 1);   // ver 2
    ProfileStore s = profileRead(n);
    Stereo st = profileFrom(s.rec);
    CHECK(s.clean && sameStereo(st, def));
    st.mode = MODE_RED_GREEN;
    st.swapEyes = true;
    st.depthPx = 3.5f;
    st.gainL = 0.8f;
    st.gainR = 0.6f;   // the record's own values
    CHECK(profileWrite(n, s, profileTo(st)) == 1 && n.order.back() == "ver" && n.clears == 0);
    CHECK(sameStereo(profileFrom(profileRead(n).rec), st));
  }
  // A key of ours with another type, under both behaviours: the write erases
  // first, and the profile reads back.
  for (int legacy = 0; legacy < 2; legacy++) {
    FakeNvs n;
    n.legacy = legacy == 1;
    putRecord(n, PK_DEPTH, 2);
    ProfileStore s = profileRead(n);
    CHECK(!s.clean);
    Stereo st;
    st.depthPx = 3.0f;
    CHECK(profileWrite(n, s, profileTo(st)) == PK_COUNT && n.clears == 1 && s.clean);
    const ProfileStore back = profileRead(n);
    CHECK(back.clean && sameStereo(profileFrom(back.rec), st));
  }
  // The negative control: without the erase, the legacy behaviour keeps the
  // old entry in front and the new depth never reads back.
  {
    FakeNvs n;
    n.legacy = true;
    putRecord(n, PK_DEPTH, 2);
    ProfileStore s = profileRead(n);
    s.clean = true;
    Stereo st;
    st.depthPx = 3.0f;
    CHECK(profileWrite(n, s, profileTo(st)) > 0 && n.clears == 0);
    CHECK(profileFrom(profileRead(n).rec).depthPx == def.depthPx);
  }
  // A write refused half way: -1, and the next one starts over, whole.
  {
    FakeNvs n;
    ProfileStore s;
    Stereo st;
    st.depthPx = 4.0f;
    st.gainL = 0.5f;
    n.putsLeft = 2;
    CHECK(profileWrite(n, s, profileTo(st)) == -1 && !s.clean);
    CHECK(!n.isKey("ver") && sameStereo(profileFrom(profileRead(n).rec), def));   // no ver: nothing half-used
    n.putsLeft = 1000;
    CHECK(profileWrite(n, s, profileTo(st)) == PK_COUNT && n.clears == 1 && s.clean);
    CHECK(sameStereo(profileFrom(profileRead(n).rec), st));
  }
  {
    FakeNvs n;   // an erase refused: nothing is put over the old record
    ProfileStore s;
    s.clean = false;
    n.clearFails = true;
    CHECK(profileWrite(n, s, profileTo(def)) == -1 && !s.clean && n.puts == 0);
  }
  // The reset: nothing stored, so the defaults; the next write is whole again.
  {
    FakeNvs n;
    ProfileStore s;
    Stereo st;
    st.mode = MODE_MONO;
    CHECK(profileWrite(n, s, profileTo(st)) == PK_COUNT);
    CHECK(profileErase(n, s) && profileStoreEmpty(s) && n.e.empty());
    const ProfileStore back = profileRead(n);
    CHECK(profileStoreEmpty(back) && sameStereo(profileFrom(back.rec), def));
    CHECK(profileWrite(n, s, profileTo(st)) == PK_COUNT);
    n.clearFails = true;
    CHECK(!profileErase(n, s) && !s.clean);
  }

  // The deferred write: not before the settle, then due, across the wrap; a
  // second change starts the wait again.
  {
    ProfileTimer idle;
    CHECK(!idle.due(0) && !idle.due(123456u));
    const uint32_t starts[] = {0u, 1000u, 0xFFFFFFFFu - 1000u, 0xFFFFFFFFu};
    for (uint32_t t0 : starts) {
      ProfileTimer t;
      t.touch(t0);
      CHECK(!t.due(t0) && !t.due(t0 + kProfileSettleMs - 1u));
      CHECK(t.due(t0 + kProfileSettleMs) && t.due(t0 + kProfileSettleMs + 60000u));
      t.touch(t0 + 2000u);
      CHECK(!t.due(t0 + kProfileSettleMs) && t.due(t0 + 2000u + kProfileSettleMs));
    }
  }

  // Keys of ours of another type and nothing else: not empty, and a reset erases them.
  {
    FakeNvs n;
    n.put("depth", FakeNvs::T_I32, 7);
    ProfileKeeper k;
    k.nvs = profileRead(n);
    CHECK(k.resetNeedsNvs());
    Stereo st;
    ProfileArgs r;
    r.reset = true;
    k.apply(r, st, &n, 0);
    CHECK(n.e.empty() && n.clears == 1 && std::strcmp(k.state(), "kept") == 0);
  }

  // The keeper: the firmware's glue. A change waits for the settle.
  const FakeNvs *none = nullptr;
  {
    FakeNvs n;
    ProfileKeeper k;
    Stereo st;
    CHECK(std::strcmp(k.state(), "kept") == 0 && !k.due(0, false));
    ProfileArgs a;
    a.depth = 3.0f;
    k.apply(a, st, const_cast<FakeNvs *>(none), 1000);
    CHECK(st.depthPx == 3.0f && std::strcmp(k.state(), "pending") == 0);
    CHECK(!k.due(1000 + kProfileSettleMs - 1, false) && k.due(1000 + kProfileSettleMs, false));
    // The bench borrows the profile: nothing is written while it runs, and
    // what is written after is the owner's, which benchEnd put back.
    const Stereo owner = st;
    st.mode = MODE_MONO;
    CHECK(!k.due(1000 + kProfileSettleMs, true) && !k.due(60000, true));
    st = owner;
    CHECK(k.due(60000, false) && k.save(st, &n, 60000) == PK_COUNT && std::strcmp(k.state(), "kept") == 0);
    CHECK(sameStereo(profileFrom(profileRead(n).rec), owner) && !k.due(120000, false));
    // A request that changes nothing NVS would hold starts no wait.
    ProfileArgs same;
    same.depth = 3.001f;
    same.mode = MODE_RED_BLUE;
    k.apply(same, st, const_cast<FakeNvs *>(none), 61000);
    k.apply(ProfileArgs(), st, const_cast<FakeNvs *>(none), 61000);
    CHECK(!k.timer.pending && std::strcmp(k.state(), "kept") == 0);
    // Depth as NVS keeps it: 2.345 shows as 2.35 at once, not after a reboot.
    ProfileArgs d;
    d.depth = 2.345f;
    k.apply(d, st, const_cast<FakeNvs *>(none), 62000);
    CHECK(st.depthPx == 2.35f);
    CHECK(k.save(st, &n, 70000) == 1 && profileFrom(profileRead(n).rec).depthPx == st.depthPx);
    // Reset first, then the rest: profile=reset&depth=3 is the defaults with depth 3.
    ProfileArgs r;
    r.reset = true;
    r.depth = 3.0f;
    CHECK(k.resetNeedsNvs());
    k.apply(r, st, &n, 80000);
    Stereo want;
    want.depthPx = 3.0f;
    CHECK(sameStereo(st, want) && n.e.empty() && std::strcmp(k.state(), "pending") == 0);
    CHECK(k.save(st, &n, 90000) == PK_COUNT && sameStereo(profileFrom(profileRead(n).rec), want));
    // A reset alone: the defaults, nothing stored, nothing waiting.
    ProfileArgs r2;
    r2.reset = true;
    k.apply(r2, st, &n, 100000);
    CHECK(sameStereo(st, def) && n.e.empty() && std::strcmp(k.state(), "kept") == 0 && !k.resetNeedsNvs());
    const int clears = n.clears;   // and with nothing stored, a reset touches nothing
    k.apply(r2, st, &n, 110000);
    CHECK(n.clears == clears && std::strcmp(k.state(), "kept") == 0);
  }
  // Refusals: tried again after each settle, kProfileRetries times, then failed
  // until the next change; a working NVS then writes the record whole.
  {
    FakeNvs n;
    ProfileKeeper k;
    Stereo st;
    ProfileArgs a;
    a.gl = 50;
    k.apply(a, st, const_cast<FakeNvs *>(none), 0);
    n.putsLeft = 0;
    uint32_t t = kProfileSettleMs;
    for (int i = 0; i < kProfileRetries; i++, t += kProfileSettleMs)
      CHECK(k.due(t, false) && k.save(st, &n, t) == -1 && std::strcmp(k.state(), "pending") == 0);
    CHECK(k.due(t, false) && k.save(st, &n, t) == -1 && std::strcmp(k.state(), "failed") == 0);
    CHECK(!k.due(t + 3600000u, false));
    CHECK(k.save(st, const_cast<FakeNvs *>(none), t) == -1);   // a namespace that will not open counts the same
    n.putsLeft = 1000;
    ProfileArgs b;
    b.gr = 40;
    k.apply(b, st, const_cast<FakeNvs *>(none), t + 1000);
    CHECK(std::strcmp(k.state(), "pending") == 0);
    CHECK(k.save(st, &n, t + 1000 + kProfileSettleMs) == PK_COUNT && std::strcmp(k.state(), "kept") == 0);
    CHECK(sameStereo(profileFrom(profileRead(n).rec), st) && sameStereo(k.afterReboot(), st));
  }
  // A refused erase: the defaults at once, and the erase tried again as a
  // write of the defaults.
  {
    FakeNvs n;
    ProfileKeeper k;
    Stereo st;
    st.mode = MODE_MONO;
    CHECK(k.save(st, &n, 0) == PK_COUNT);
    n.clearFails = true;
    ProfileArgs r;
    r.reset = true;
    k.apply(r, st, &n, 1000);
    CHECK(sameStereo(st, def) && std::strcmp(k.state(), "pending") == 0 && k.failed);
    CHECK(k.afterReboot().mode == MODE_MONO);   // until the retry lands
    uint32_t t = 1000 + kProfileSettleMs;
    CHECK(k.due(t, false) && k.save(st, &n, t) == -1 && std::strcmp(k.state(), "pending") == 0);   // refused again: again later
    n.clearFails = false;
    t += kProfileSettleMs;
    CHECK(k.due(t, false) && k.save(st, &n, t) == PK_COUNT);
    CHECK(sameStereo(profileFrom(profileRead(n).rec), def) && std::strcmp(k.state(), "kept") == 0);
  }
}

int main(int argc, char **argv) {
  profile();
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
  continuity();
  runs();
  landscape();
  reprojectMatches();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
