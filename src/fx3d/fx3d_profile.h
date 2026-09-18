#pragma once
// The glasses profile - what the owner sets with the calibration - kept in NVS
// so a reboot does not undo it. Plain C++11: the reading and writing rules, and
// the glue the firmware runs (ProfileKeeper), are tested on the host
// (tools/fx3d/check_fx3d.py) against a stand-in for NVS.
//
// NVS namespace "fx3d", one typed key per value, so any NVS dump shows them:
//
//   key    type  meaning                                              range
//   mode   u8    0 mono, 1 red-blue, 2 red-cyan, 3 red-green          0..3
//   swap   u8    1: the red channel shows the right eye's picture     0..1
//   depth  u16   the largest disparity, in 1/100 of a pixel           0..1600
//   gl     u8    the left eye's gain, per cent                        0..100
//   gr     u8    the right eye's gain, per cent                       0..100
//   ver    u8    this layout; written last                            1
//
// Nothing stored means the defaults (Stereo() in fx3d_model.h): red-blue, the
// eyes as they are, 2 px, both eyes at 100 %.
//
// Reading never refuses. A key that is missing, of another type, or out of its
// range gives that value's default; a `ver` other than 1, or none, gives every
// default. The panel lives in a wall: a bad record costs a calibration, not a
// boot.
//
// Writing touches only the keys whose value changed; each put commits
// (Preferences::putUChar, arduino-esp32 2.0.17). A key of ours found with
// another type is the one case that needs more: in ESP-IDF 4.4.7, which
// 2.0.17 is built on, a put of another type does not replace the old entry.
// Page::findItem stops at the old entry with a type mismatch and
// Storage::findItem moves on, so Storage::writeItem adds the new entry and
// erases nothing; and since the index hash covers the namespace, key and
// chunk but not the type (Item::calculateCrc32WithoutValue), reads on that
// page keep stopping at the old one (nvs_page.cpp, nvs_storage.cpp,
// nvs_types.cpp at v4.4.7). IDF's own tests call this the legacy behaviour
// (host_test/nvs_host_test/main/test_nvs.cpp,
// CONFIG_NVS_LEGACY_DUP_KEYS_COMPATIBILITY). So such a record is erased first
// ("erase all key-value pairs in a namespace", nvs_erase_all) and written
// whole.

#include "fx3d_model.h"

namespace fx3d {

const uint8_t kProfileVersion = 1;
const uint16_t kDepthMax100 = 1600;   // /api/fx3d takes 0..16 px
const uint8_t kNone8 = 0xFF;          // what a read gives for a u8 key that is not there as a u8
const uint16_t kNone16 = 0xFFFF;      // the same for the u16

// The order is the order of writing, `ver` last: a first write, or one after
// an erase, that is cut short leaves no `ver`, so none of it is used. A later
// write cut short can leave some keys new and some old, each in its range.
enum ProfileKey { PK_MODE, PK_SWAP, PK_DEPTH, PK_GL, PK_GR, PK_VER, PK_COUNT };
inline const char *profileKeyName(int k) {
  static const char *const n[PK_COUNT] = {"mode", "swap", "depth", "gl", "gr", "ver"};
  return (k >= 0 && k < PK_COUNT) ? n[k] : "";
}
inline bool profileKeyWide(int k) { return k == PK_DEPTH; }   // u16; the rest are u8
inline uint16_t profileNone(int k) { return profileKeyWide(k) ? kNone16 : kNone8; }

// The raw values, as NVS holds them; kNone8/kNone16 where nothing readable is.
struct StoredProfile {
  uint16_t v[PK_COUNT];
  StoredProfile() {
    for (int k = 0; k < PK_COUNT; k++) v[k] = profileNone(k);
  }
};
inline bool sameRecord(const StoredProfile &a, const StoredProfile &b) {
  for (int k = 0; k < PK_COUNT; k++)
    if (a.v[k] != b.v[k]) return false;
  return true;
}

// What the panel uses. A value it cannot use gives that value's default.
inline Stereo profileFrom(const StoredProfile &p) {
  Stereo st;
  if (p.v[PK_VER] != kProfileVersion) return st;
  if (p.v[PK_MODE] < MODE_COUNT) st.mode = (uint8_t)p.v[PK_MODE];
  if (p.v[PK_SWAP] <= 1) st.swapEyes = p.v[PK_SWAP] == 1;
  if (p.v[PK_DEPTH] <= kDepthMax100) st.depthPx = (float)p.v[PK_DEPTH] / 100.0f;
  if (p.v[PK_GL] <= 100) st.gainL = (float)p.v[PK_GL] / 100.0f;
  if (p.v[PK_GR] <= 100) st.gainR = (float)p.v[PK_GR] / 100.0f;
  return st;
}

// How many of the five values profileFrom took from the record, for the log.
inline int profileUsed(const StoredProfile &p) {
  if (p.v[PK_VER] != kProfileVersion) return 0;
  return (p.v[PK_MODE] < MODE_COUNT) + (p.v[PK_SWAP] <= 1) + (p.v[PK_DEPTH] <= kDepthMax100) + (p.v[PK_GL] <= 100) +
         (p.v[PK_GR] <= 100);
}

// A value scaled to whole units, rounded, inside 0..hi; NaN gives 0.
inline uint16_t profileUnits(float v, float scale, uint16_t hi) {
  const float u = v * scale + 0.5f;
  if (!(u >= 0.0f)) return 0;
  return u >= (float)hi ? hi : (uint16_t)u;
}

inline StoredProfile profileTo(const Stereo &st) {
  StoredProfile p;
  p.v[PK_MODE] = st.mode < MODE_COUNT ? st.mode : (uint8_t)MODE_RED_BLUE;
  p.v[PK_SWAP] = st.swapEyes ? 1 : 0;
  p.v[PK_DEPTH] = profileUnits(st.depthPx, 100.0f, kDepthMax100);
  p.v[PK_GL] = profileUnits(st.gainL, 100.0f, 100);
  p.v[PK_GR] = profileUnits(st.gainR, 100.0f, 100);
  p.v[PK_VER] = kProfileVersion;
  return p;
}

// What NVS holds under our names, as far as this module knows. `clean` is
// false once a key of ours was seen with another type, or a write failed
// half way: the next write then erases the namespace and writes every key.
struct ProfileStore {
  StoredProfile rec;
  bool clean;
  ProfileStore() : clean(true) {}
};
inline bool profileStoreEmpty(const ProfileStore &s) { return s.clean && sameRecord(s.rec, StoredProfile()); }

// Nvs is anything with Preferences' calls: the firmware passes a Preferences
// opened on the namespace, the host test a stand-in that can hold keys of
// another type and refuse writes.
template <class Nvs>
ProfileStore profileRead(Nvs &nvs) {
  ProfileStore s;
  for (int k = 0; k < PK_COUNT; k++) {
    const char *name = profileKeyName(k);
    const uint16_t v = profileKeyWide(k) ? nvs.getUShort(name, kNone16) : nvs.getUChar(name, kNone8);
    s.rec.v[k] = v;
    // Nothing read, yet the key is there: another type (or our own none value).
    if (v == profileNone(k) && nvs.isKey(name)) s.clean = false;
  }
  return s;
}

// Returns the keys written (0 when NVS already holds `want`), or -1 when NVS
// refused a write; `s` then says to start over next time.
template <class Nvs>
int profileWrite(Nvs &nvs, ProfileStore &s, const StoredProfile &want) {
  if (!s.clean) {
    if (!nvs.clear()) return -1;
    s = ProfileStore();
  }
  int n = 0;
  for (int k = 0; k < PK_COUNT; k++) {
    const uint16_t v = want.v[k];
    if (s.rec.v[k] == v) continue;
    const char *name = profileKeyName(k);
    const size_t put = profileKeyWide(k) ? nvs.putUShort(name, v) : nvs.putUChar(name, (uint8_t)v);
    if (!put) {
      s.clean = false;
      return -1;
    }
    s.rec.v[k] = v;
    n++;
  }
  return n;
}

// The reset: nothing stored, so the defaults - today's and any later firmware's.
template <class Nvs>
bool profileErase(Nvs &nvs, ProfileStore &s) {
  if (!nvs.clear()) {
    s.clean = false;
    return false;
  }
  s = ProfileStore();
  return true;
}

// When to write: kProfileSettleMs after the last change, the clock styles'
// rule (src/control/clock_style.cpp, SETTLE_MS, chosen there for flash wear):
// a slider stepped through ten values is one write, not ten. Elapsed time,
// unsigned, so the millis() wrap does not matter.
const uint32_t kProfileSettleMs = 2500;
struct ProfileTimer {
  bool pending;
  uint32_t sinceMs;
  ProfileTimer() : pending(false), sinceMs(0) {}
  void touch(uint32_t nowMs) {
    pending = true;
    sinceMs = nowMs;
  }
  bool due(uint32_t nowMs) const { return pending && nowMs - sinceMs >= kProfileSettleMs; }
};

// A refused write is tried again after another settle, this many times; then
// the profile waits for the next change. Our choice, not a measured figure: a
// refusal that comes back three times, 2.5 s apart, is not a passing one.
// (src/railboard/railboard.cpp retries a refused open after every settle,
// without a limit.)
const uint8_t kProfileRetries = 3;

// What /api/fx3d asked of the profile, every value already checked by the
// API; negative for "not asked".
struct ProfileArgs {
  bool reset;
  int mode, swap, gl, gr;
  float depth;
  ProfileArgs() : reset(false), mode(-1), swap(-1), gl(-1), gr(-1), depth(-1.0f) {}
};

// The firmware's glue, here so the host can test it: what a request does to
// the profile, when the write may go, what a refusal does, what the API says.
// Nvs is opened by the firmware; a null one is a namespace that would not open.
struct ProfileKeeper {
  ProfileStore nvs;     // what NVS holds, as far as this module knows
  ProfileTimer timer;   // a change waiting for the owner to stop
  bool failed;          // NVS refused the last write or erase
  uint8_t retries;      // tries left after a refusal
  ProfileKeeper() : failed(false), retries(0) {}

  // A reset needs NVS only when something is stored.
  bool resetNeedsNvs() const { return !profileStoreEmpty(nvs); }

  // The profile arguments of one request. profile=reset goes first, so
  // profile=reset&depth=3 is the defaults with depth 3. Depth is rounded to
  // the 1/100 px NVS keeps, so what shows is what a reboot brings back. The
  // wait starts only if what NVS would hold changed.
  template <class Nvs>
  void apply(const ProfileArgs &a, Stereo &st, Nvs *nv, uint32_t nowMs) {
    if (a.reset) reset(st, nv, nowMs);
    const StoredProfile before = profileTo(st);
    if (a.mode >= 0 && a.mode < MODE_COUNT) st.mode = (uint8_t)a.mode;
    if (a.depth >= 0.0f) st.depthPx = (float)profileUnits(a.depth, 100.0f, kDepthMax100) / 100.0f;
    if (a.swap >= 0) st.swapEyes = a.swap != 0;
    if (a.gl >= 0) st.gainL = (float)(a.gl > 100 ? 100 : a.gl) / 100.0f;
    if (a.gr >= 0) st.gainR = (float)(a.gr > 100 ? 100 : a.gr) / 100.0f;
    if (!sameRecord(profileTo(st), before)) {
      timer.touch(nowMs);
      retries = kProfileRetries;
    }
  }

  // The write may go: the settle has passed, and the bench, which borrows the
  // profile and gives it back when it ends, is not running.
  bool due(uint32_t nowMs, bool benchRunning) const { return !benchRunning && timer.due(nowMs); }

  // The keys written, or -1 when NVS refused (a retry is then pending while
  // retries last).
  template <class Nvs>
  int save(const Stereo &st, Nvs *nv, uint32_t nowMs) {
    timer.pending = false;
    const int n = nv ? profileWrite(*nv, nvs, profileTo(st)) : -1;
    refused(n < 0, nowMs);
    return n;
  }

  // What a reboot would bring back, as far as this module knows.
  Stereo afterReboot() const { return profileFrom(nvs.rec); }

  // A retry waiting is "pending"; "failed" once the retries are spent.
  const char *state() const { return timer.pending ? "pending" : (failed ? "failed" : "kept"); }

 private:
  // The defaults at once, and nothing left stored, so a later firmware's
  // defaults apply too. A refused erase is retried as a write of the defaults.
  template <class Nvs>
  void reset(Stereo &st, Nvs *nv, uint32_t nowMs) {
    st = Stereo();
    timer.pending = false;
    retries = kProfileRetries;
    refused(!(profileStoreEmpty(nvs) || (nv && profileErase(*nv, nvs))), nowMs);
  }
  void refused(bool no, uint32_t nowMs) {
    failed = no;
    if (no && retries > 0) {
      retries--;
      timer.touch(nowMs);
    }
  }
};

}  // namespace fx3d
