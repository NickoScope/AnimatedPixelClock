#include "media_model.h"

#include <cstdio>
#include <cstring>

namespace media {

const char *const kStateKeys[ST_COUNT] = {"unavailable", "off", "idle", "paused", "playing"};
const char *const kKindKeys[K_COUNT]   = {"music", "radio", "tts"};

// ── UTF-8 ───────────────────────────────────────────────────────────────────
// One code point at p: its length, and *cp. A broken or overlong sequence, a
// surrogate or anything past U+10FFFF is length 1 with *cp = 0xFFFD, so the
// caller steps over one byte and never reads past a NUL.
static size_t decode(const unsigned char *p, uint32_t *cp) {
  const unsigned char c = p[0];
  if (c < 0x80) { *cp = c; return 1; }
  size_t n;
  uint32_t v, least;
  if      ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1Fu; least = 0x80; }
  else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0Fu; least = 0x800; }
  else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07u; least = 0x10000; }
  else { *cp = 0xFFFD; return 1; }
  for (size_t i = 1; i < n; i++) {
    if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }   // a NUL stops here too
    v = (v << 6) | (p[i] & 0x3Fu);
  }
  if (v < least || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { *cp = 0xFFFD; return 1; }
  *cp = v;
  return n;
}

size_t copyUtf8(char *dst, size_t cap, const char *src) {
  if (!dst || !cap) return 0;
  size_t k = 0;
  const unsigned char *p = (const unsigned char *)(src ? src : "");
  while (*p) {
    uint32_t cp;
    const size_t n = decode(p, &cp);
    if (n == 1 && *p >= 0x80) {                         // broken
      if (k + 1 >= cap) break;
      dst[k++] = '?';
    } else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
      if (k + 1 >= cap) break;
      dst[k++] = ' ';
    } else {
      if (k + n >= cap) break;                          // whole code points only
      memcpy(dst + k, p, n);
      k += n;
    }
    p += n;
  }
  dst[k] = '\0';
  return k;
}

// ── ids ─────────────────────────────────────────────────────────────────────
bool validPlayerId(const char *s) {
  static const char kPrefix[] = "media_player.";
  const size_t pre = sizeof(kPrefix) - 1;
  if (!s || strncmp(s, kPrefix, pre)) return false;
  size_t n = 0;
  for (const char *r = s + pre; r[n]; n++) {
    const char c = r[n];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return n > 0 && pre + n < kPlayerIdLen;
}

bool validFavId(const char *s) {
  if (!s || !*s) return false;
  size_t n = 0;
  for (; s[n]; n++) {
    const unsigned char c = (unsigned char)s[n];
    if (c < 0x21 || c > 0x7E || c == '"' || c == '\\') return false;
  }
  return n < kFavIdLen;
}

// Up to `max` of A-Z 0-9, plus '_' when `underscore`. Empty is valid.
static bool tagOk(const char *s, size_t max, bool underscore) {
  size_t n = 0;
  for (; s[n]; n++) {
    const char c = s[n];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (underscore && c == '_'))) return false;
  }
  return n <= max;
}

// ── JSON fields ─────────────────────────────────────────────────────────────
// A present field must be a string; absent or null reads as "".
static bool optStr(JsonVariantConst v, const char **out) {
  if (v.isNull()) { *out = ""; return true; }
  if (!v.is<const char *>()) return false;
  const char *s = v.as<const char *>();
  *out = s ? s : "";
  return true;
}

// A present field must be a whole number in [0, max]; absent or null reads 0.
static bool optU32(JsonVariantConst v, uint32_t max, uint32_t *out) {
  if (v.isNull()) { *out = 0; return true; }
  if (v.is<bool>() || !v.is<uint32_t>()) return false;
  const uint32_t x = v.as<uint32_t>();
  if (x > max) return false;
  *out = x;
  return true;
}

static int keyIndex(const char *s, const char *const *keys, int n) {
  for (int i = 0; i < n; i++)
    if (!strcmp(s, keys[i])) return i;
  return -1;
}

// v must be the schema, ts an epoch after 2001-09-09 (1e9): a clock that has
// never been set sends 0 or a small number, and that is not a timestamp.
static const char *header(JsonObjectConst in, uint32_t *ts) {
  if (in.isNull()) return "not an object";
  JsonVariantConst v = in["v"];
  if (v.is<bool>() || !v.is<long>() || v.as<long>() != kSchema) return "v";
  JsonVariantConst t = in["ts"];
  if (t.is<bool>() || !t.is<uint32_t>() || t.as<uint32_t>() < 1000000000UL) return "ts";
  *ts = t.as<uint32_t>();
  return nullptr;
}

// The id less its domain, for a list entry that came without a name.
static const char *idTail(const char *id) {
  const char *dot = strchr(id, '.');
  return dot ? dot + 1 : id;
}

const char *stateFrom(JsonObjectConst in, NowPlaying *out) {
  NowPlaying &n = *out;
  memset(&n, 0, sizeof(n));
  if (const char *e = header(in, &n.ts)) return e;

  const char *player, *name, *st, *kind, *title, *artist, *album, *src, *err;
  if (!optStr(in["player"], &player) || (player[0] && !validPlayerId(player))) return "player";
  if (!optStr(in["name"], &name)) return "name";
  if (!optStr(in["st"], &st)) return "st";
  // No player followed carries no state of its own.
  const int si = st[0] ? keyIndex(st, kStateKeys, ST_COUNT) : (player[0] ? -1 : (int)ST_UNAVAILABLE);
  if (si < 0) return "st";
  if (!optStr(in["kind"], &kind)) return "kind";
  const int ki = kind[0] ? keyIndex(kind, kKindKeys, K_COUNT) : (int)K_MUSIC;
  if (ki < 0) return "kind";
  if (!optStr(in["title"], &title)) return "title";
  if (!optStr(in["artist"], &artist)) return "artist";
  if (!optStr(in["album"], &album)) return "album";
  if (!optStr(in["src"], &src) || !tagOk(src, kSrcLen - 1, false)) return "src";
  if (!optStr(in["err"], &err) || !tagOk(err, kErrLen - 1, true)) return "err";

  JsonVariantConst jv = in["vol"];
  n.vol = -1;
  if (!jv.isNull()) {
    if (jv.is<bool>() || !jv.is<long>() || jv.as<long>() < 0 || jv.as<long>() > 100) return "vol";
    n.vol = (int8_t)jv.as<long>();
  }
  JsonVariantConst jm = in["muted"];
  if (!jm.isNull()) {
    if (!jm.is<bool>()) return "muted";
    n.muted = jm.as<bool>();
  }
  if (!optU32(in["pos"], kMaxSeconds, &n.pos)) return "pos";
  if (!optU32(in["dur"], kMaxSeconds, &n.dur)) return "dur";
  if (!optU32(in["pos_at"], 0xFFFFFFFFUL, &n.posAt)) return "pos_at";

  copyUtf8(n.player, sizeof(n.player), player);
  copyUtf8(n.name, sizeof(n.name), name[0] ? name : (player[0] ? idTail(player) : ""));
  copyUtf8(n.src, sizeof(n.src), src[0] ? src : "HA");
  copyUtf8(n.err, sizeof(n.err), err);
  copyUtf8(n.title, sizeof(n.title), title);
  copyUtf8(n.artist, sizeof(n.artist), artist);
  copyUtf8(n.album, sizeof(n.album), album);
  n.st   = (uint8_t)si;
  n.kind = (uint8_t)ki;
  n.have = true;
  return nullptr;
}

static void countDrop(uint8_t *dropped) {
  if (*dropped < 255) (*dropped)++;
}

const char *playersFrom(JsonObjectConst in, Players *out) {
  Players &l = *out;
  memset(&l, 0, sizeof(l));
  if (const char *e = header(in, &l.ts)) return e;
  const char *sel;
  if (!optStr(in["sel"], &sel) || (sel[0] && !validPlayerId(sel))) return "sel";
  JsonArrayConst list = in["list"].as<JsonArrayConst>();
  if (list.isNull()) return "list";
  copyUtf8(l.sel, sizeof(l.sel), sel);
  for (JsonVariantConst item : list) {
    JsonObjectConst o = item.as<JsonObjectConst>();
    const char *id = o["id"].is<const char *>() ? o["id"].as<const char *>() : nullptr;
    const char *name, *st;
    int si = -1;
    if (!o.isNull() && id && validPlayerId(id) && optStr(o["name"], &name) && optStr(o["st"], &st))
      si = st[0] ? keyIndex(st, kStateKeys, ST_COUNT) : (int)ST_UNAVAILABLE;
    bool dup = false;
    for (uint8_t i = 0; si >= 0 && i < l.n; i++) dup = dup || !strcmp(l.p[i].id, id);
    if (si < 0 || dup || l.n >= kMaxPlayers) { countDrop(&l.dropped); continue; }
    Player &p = l.p[l.n++];
    copyUtf8(p.id, sizeof(p.id), id);
    copyUtf8(p.name, sizeof(p.name), name[0] ? name : idTail(id));
    p.st = (uint8_t)si;
  }
  l.have = true;
  return nullptr;
}

const char *favsFrom(JsonObjectConst in, Favs *out) {
  Favs &l = *out;
  memset(&l, 0, sizeof(l));
  if (const char *e = header(in, &l.ts)) return e;
  JsonArrayConst list = in["list"].as<JsonArrayConst>();
  if (list.isNull()) return "list";
  for (JsonVariantConst item : list) {
    JsonObjectConst o = item.as<JsonObjectConst>();
    const char *id = o["id"].is<const char *>() ? o["id"].as<const char *>() : nullptr;
    const char *name = "";
    bool ok = !o.isNull() && id && validFavId(id) && optStr(o["name"], &name);
    for (uint8_t i = 0; ok && i < l.n; i++) ok = strcmp(l.f[i].id, id) != 0;
    if (!ok || l.n >= kMaxFavs) { countDrop(&l.dropped); continue; }
    Fav &f = l.f[l.n++];
    copyUtf8(f.id, sizeof(f.id), id);
    copyUtf8(f.name, sizeof(f.name), name[0] ? name : id);
  }
  l.have = true;
  return nullptr;
}

// ── transliteration ─────────────────────────────────────────────────────────
// Cyrillic: ICAO Doc 9303, Machine Readable Travel Documents, Eighth Edition
// 2021, Part 3, Section 6.B "Transliteration of Cyrillic Characters", read from
// icao.int on 2026-09-14. The table gives capitals; small letters take the same
// Latin. Its per-language exceptions (Ukrainian first letters, Serbian,
// Belarusian, Bulgarian, Macedonian) are not applied: a title does not say its
// language. Not in the table, and so chosen here: U+0400 E, U+0403 G, U+040B C,
// U+040D I; the soft sign U+042C is not in the table and is dropped.
static const char *const kCyr[0x30] = {
  "E",  "E",  "D",  "G",  "IE", "DZ", "I",  "I",  "J",  "LJ", "NJ", "C",  "K",  "I",  "U",  "DZ",   // U+0400
  "A",  "B",  "V",  "G",  "D",  "E",  "ZH", "Z",  "I",  "I",  "K",  "L",  "M",  "N",  "O",  "P",    // U+0410
  "R",  "S",  "T",  "U",  "F",  "KH", "TS", "CH", "SH", "SHCH", "IE", "Y", "",  "E",  "IU", "IA",   // U+0420
};

// The other letters the table lists, capital then small.
static const char *cyrExtra(uint32_t cp) {
  switch (cp) {
  case 0x046A: case 0x046B: return "U";
  case 0x0474: case 0x0475: return "Y";
  case 0x0490: case 0x0491: return "G";
  case 0x0492: case 0x0493: return "G";
  case 0x04BA: case 0x04BB: return "C";
  default:                  return nullptr;
  }
}

// Latin-1 Supplement and Latin Extended-A (U+00C0..U+017F) as base capitals;
// '?' marks those handled by latinTwo() or not letters. The same fold as the
// world clock's names (worldclock.cpp), with the case kept here.
static const char kLatinBase[] =
  "AAAAAA?CEEEEIIII" "DNOOOOO?OUUUUY??"   // U+00C0
  "AAAAAA?CEEEEIIII" "DNOOOOO?OUUUUY?Y"   // U+00E0
  "AAAAAACCCCCCCCDD" "DDEEEEEEEEEEGGGG"   // U+0100
  "GGGGHHHHIIIIIIII" "II??JJKKKLLLLLLL"   // U+0120
  "LLLNNNNNNNNNOOOO" "OO??RRRRRRSSSSSS"   // U+0140
  "SSTTTTTTUUUUUUUU" "UUUUWWYYYZZZZZZS";  // U+0160

static const char *latinTwo(uint32_t cp) {
  switch (cp) {
  case 0x00C6: case 0x00E6: return "AE";
  case 0x00DE: case 0x00FE: return "TH";
  case 0x00DF:              return "SS";
  case 0x0132: case 0x0133: return "IJ";
  case 0x0152: case 0x0153: return "OE";
  default:                  return nullptr;
  }
}

static bool isLetter(uint32_t cp) {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x17F && cp != 0xD7 && cp != 0xF7) ||
         (cp >= 0x0400 && cp <= 0x04FF);
}

// Capital or not, for the letters above. Latin Extended-A alternates capital
// and small, with the parity shifting at U+0138/U+0149/U+0178.
static bool isUpper(uint32_t cp) {
  if (cp < 0x80) return cp >= 'A' && cp <= 'Z';
  if (cp >= 0xC0 && cp <= 0xDE) return cp != 0xD7;
  if (cp >= 0xDF && cp <= 0xFF) return false;
  if (cp >= 0x100 && cp <= 0x137) return (cp & 1) == 0;
  if (cp >= 0x139 && cp <= 0x148) return (cp & 1) == 1;
  if (cp >= 0x14A && cp <= 0x177) return (cp & 1) == 0;
  if (cp == 0x178) return true;
  if (cp >= 0x179 && cp <= 0x17E) return (cp & 1) == 1;
  if (cp >= 0x0400 && cp <= 0x042F) return true;
  if (cp >= 0x0430 && cp <= 0x045F) return false;
  if (cp >= 0x0460 && cp <= 0x04FF) return (cp & 1) == 0;
  return false;
}

// What to write for one code point: a replacement string, or nullptr to drop
// it, or kUnknown.
static const char kUnknown[] = "?";

static const char *mapPunct(uint32_t cp) {
  switch (cp) {
  case 0x00A0: case 0x202F: case 0x205F: case 0x3000: return " ";
  case 0x00AB: case 0x00BB: case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "\"";
  case 0x00B4: case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return "'";
  case 0x00B7: case 0x2022: return "-";
  case 0x00A9: return "(C)";
  case 0x00AE: return "(R)";
  case 0x00D7: return "x";
  case 0x2026: return "...";
  case 0x2116: return "No";
  case 0x2122: return "TM";
  case 0xFFFD: return kUnknown;
  default: break;
  }
  if (cp >= 0x2010 && cp <= 0x2015) return "-";
  if (cp >= 0x2000 && cp <= 0x200A) return " ";
  return kUnknown;
}

static bool dropped(uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) ||    // combining diacritics
         (cp >= 0x200B && cp <= 0x200F) ||    // zero width, direction marks
         cp == 0x2060 || cp == 0xFEFF ||
         (cp >= 0x20D0 && cp <= 0x20FF) ||    // combining marks for symbols
         (cp >= 0x2190 && cp <= 0x21FF) ||    // arrows
         (cp >= 0x2600 && cp <= 0x27BF) ||    // symbols and dingbats
         (cp >= 0x2B00 && cp <= 0x2BFF) ||    // more arrows and stars
         (cp >= 0xFE00 && cp <= 0xFE0F) ||    // variation selectors
         (cp >= 0x1F000 && cp <= 0x1FAFF) ||  // emoji, skin tones included
         (cp >= 0xE0000 && cp <= 0xE007F);    // tags
}

namespace {
struct Sink {
  char  *o;
  size_t cap, k;
  bool   space, question;
  void put(char c) {
    if (c == ' ' && (k == 0 || space)) return;   // no leading run, no doubled space
    if (c == '?' && question) return;            // one '?' for a run of unknowns
    if (k + 1 >= cap) return;
    o[k++] = c;
    space = c == ' ';
    question = c == '?';
  }
};
}  // namespace

size_t translit(const char *utf8, char *out, size_t cap) {
  if (!out || !cap) return 0;
  Sink s{out, cap, 0, false, false};
  const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : "");
  uint32_t prev = 0;   // the previous code point that was a letter, 0 = none since a non-letter
  while (*p) {
    uint32_t cp;
    p += decode(p, &cp);
    const char *rep = nullptr;
    bool letter = false;
    char single[2] = {0, 0};
    if (cp < 0x80) {
      single[0] = (cp < 0x20 || cp == 0x7F) ? ' ' : (char)cp;
      rep = single;
      letter = isLetter(cp);
    } else if (cp >= 0x80 && cp < 0xA0) {
      rep = " ";
    } else if (cp >= 0x0400 && cp <= 0x045F) {
      const uint32_t base = cp >= 0x0450 ? cp - 0x50 : (cp >= 0x0430 ? cp - 0x20 : cp);
      rep = kCyr[base - 0x0400];
      letter = true;
    } else if (const char *x = cyrExtra(cp)) {
      rep = x;
      letter = true;
    } else if (const char *two = latinTwo(cp)) {
      rep = two;
      letter = true;
    } else if (cp >= 0xC0 && cp <= 0x17F && kLatinBase[cp - 0xC0] != '?') {
      single[0] = kLatinBase[cp - 0xC0];
      rep = single;
      letter = true;
    } else if (dropped(cp)) {
      rep = nullptr;
    } else {
      rep = mapPunct(cp);
    }

    if (rep && letter && cp >= 0x80) {
      // Case. A small letter writes small. A capital writes one capital, or all
      // capitals when a neighbouring letter is a capital too: Shchi, SHCHI.
      const size_t len = strlen(rep);
      bool allCaps = true;
      if (!isUpper(cp)) {
        allCaps = false;
      } else if (len > 1) {
        uint32_t next;
        decode(p, &next);
        allCaps = (*p && isLetter(next)) ? isUpper(next) : (prev && isUpper(prev));
      }
      for (size_t i = 0; i < len; i++) {
        const char c = rep[i];
        const bool up = isUpper(cp) && (i == 0 || allCaps);
        s.put(up ? c : (char)(c >= 'A' && c <= 'Z' ? c + 32 : c));
      }
    } else if (rep) {
      for (const char *c = rep; *c; c++) s.put(*c);
    }
    if (rep || !dropped(cp)) prev = letter ? cp : 0;   // a dropped mark keeps the word together
  }
  while (s.k && out[s.k - 1] == ' ') s.k--;
  out[s.k] = '\0';
  return s.k;
}

// ── the track and its position ──────────────────────────────────────────────
static uint32_t fnv(uint32_t h, const char *s) {
  for (; *s; s++) {
    h ^= (uint8_t)*s;
    h *= 16777619u;
  }
  h ^= 0x1F;                // a separator, so "ab"+"c" differs from "a"+"bc"
  return h * 16777619u;
}

uint32_t trackKey(const NowPlaying &np) {
  uint32_t h = 2166136261u;
  h = fnv(h, np.player);
  h = fnv(h, np.title);
  h = fnv(h, np.artist);
  return fnv(h, np.album);
}

uint32_t positionAt(const NowPlaying &np, int64_t now) {
  uint64_t p = np.pos;
  if (np.st == ST_PLAYING && np.posAt && now > (int64_t)np.posAt) p += (uint64_t)(now - (int64_t)np.posAt);
  if (np.dur && p > np.dur) p = np.dur;
  if (p > kMaxSeconds) p = kMaxSeconds;
  return (uint32_t)p;
}

void formatClock(char *out, size_t cap, uint32_t s) {
  if (s >= 3600) snprintf(out, cap, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  else           snprintf(out, cap, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// ── commands ────────────────────────────────────────────────────────────────
// snprintf is enough: every string that goes in is a literal or an id from a
// closed alphabet with no quote and no backslash (validPlayerId, validFavId).
static size_t fit(int n, size_t cap) { return (n > 0 && (size_t)n < cap) ? (size_t)n : 0; }

size_t cmdSimple(char *out, size_t cap, const char *cmd, const char *player) {
  static const char *const kAllowed[] = {"toggle", "play", "pause", "next", "prev"};
  if (!cmd || keyIndex(cmd, kAllowed, 5) < 0 || !validPlayerId(player)) return 0;
  return fit(snprintf(out, cap, "{\"v\":1,\"cmd\":\"%s\",\"player\":\"%s\"}", cmd, player), cap);
}

size_t cmdInt(char *out, size_t cap, const char *cmd, const char *player, long value) {
  if (!cmd || !validPlayerId(player)) return 0;
  if (!strcmp(cmd, "vol")) {
    if (value < 0 || value > 100) return 0;
  } else if (!strcmp(cmd, "vol_step")) {
    if (value < -20 || value > 20 || value == 0) return 0;
  } else {
    return 0;
  }
  return fit(snprintf(out, cap, "{\"v\":1,\"cmd\":\"%s\",\"player\":\"%s\",\"value\":%ld}", cmd, player, value), cap);
}

size_t cmdMute(char *out, size_t cap, const char *player, bool muted) {
  if (!validPlayerId(player)) return 0;
  return fit(snprintf(out, cap, "{\"v\":1,\"cmd\":\"mute\",\"player\":\"%s\",\"value\":%s}", player,
                      muted ? "true" : "false"), cap);
}

size_t cmdFav(char *out, size_t cap, const char *player, const char *favId) {
  if (!validPlayerId(player) || !validFavId(favId)) return 0;
  return fit(snprintf(out, cap, "{\"v\":1,\"cmd\":\"play_fav\",\"player\":\"%s\",\"id\":\"%s\"}", player, favId), cap);
}

size_t selectPayload(char *out, size_t cap, const char *player) {
  if (!validPlayerId(player)) return 0;
  return fit(snprintf(out, cap, "{\"v\":1,\"player\":\"%s\"}", player), cap);
}

}  // namespace media
