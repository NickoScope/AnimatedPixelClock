// Host test for src/media/media_model.{h,cpp}: payload parsing and bounds,
// transliteration, progress extrapolation, the track key and the command
// payloads. Built and run by tools/media/check_media.py.
//
//   media_host_test                   the tests
//   media_host_test translit          stdin lines -> transliterated lines (tools/media/render.py)
//   media_host_test parse LEAF FILE   one payload through the panel's parser: OK, or REFUSED and why

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "media_model.h"

using namespace media;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQS(a, b) do { const std::string x_(a), y_(b); if (x_ == y_) g_pass++; else { g_fail++; \
  std::printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, x_.c_str(), y_.c_str()); } } while (0)

// The firmware's allocator, less the cap and the PSRAM: counts the peak.
class Counting : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t n) override { return reallocate(nullptr, n); }
  void deallocate(void *p) override {
    if (!p) return;
    Hdr *h = static_cast<Hdr *>(p) - 1;
    used -= h->size;
    std::free(h);
  }
  void *reallocate(void *p, size_t n) override {
    Hdr *old = p ? static_cast<Hdr *>(p) - 1 : nullptr;
    const size_t was = old ? old->size : 0;
    Hdr *h = static_cast<Hdr *>(std::realloc(old, sizeof(Hdr) + n));
    if (!h) return nullptr;
    h->size = n;
    used = used - was + n;
    if (used > peak) peak = used;
    return h + 1;
  }
  size_t used = 0, peak = 0;

 private:
  union Hdr { size_t size; max_align_t align; };
};

enum Leaf { L_STATE, L_PLAYERS, L_FAVS };

static NowPlaying g_np;
static Players g_pl;
static Favs g_fv;

// Through deserializeJson(const char*, len), as media_ha.cpp does.
static std::string parse(Leaf leaf, const std::string &json, size_t *peak = nullptr) {
  Counting a;
  std::string r;
  {
    JsonDocument doc(&a);
    if (deserializeJson(doc, json.data(), json.size())) return "unparsable";
    const JsonObjectConst o = doc.as<JsonObjectConst>();
    const char *why = leaf == L_STATE ? stateFrom(o, &g_np) : (leaf == L_PLAYERS ? playersFrom(o, &g_pl) : favsFrom(o, &g_fv));
    r = why ? why : "";
  }
  if (peak) *peak = a.peak;
  return r;
}

static const char *kState =
  "{\"v\":1,\"ts\":1789412345,\"player\":\"media_player.nickoscope32_audio_s3\",\"name\":\"NickoScope32 Audio S3\","
  "\"src\":\"MA\",\"st\":\"playing\",\"kind\":\"music\",\"title\":\"Olive Tree\",\"artist\":\"Quinn XCII\","
  "\"album\":\"Olive Tree\",\"vol\":50,\"muted\":false,\"pos\":56,\"pos_at\":1789412300,\"dur\":241}";

// kState with one key set to a raw JSON value, or removed when raw is null.
static std::string stateWith(const char *key, const char *raw) {
  JsonDocument doc;
  deserializeJson(doc, kState);
  if (raw) doc[key] = serialized(std::string(raw));
  else     doc.remove(key);
  std::string out;
  serializeJson(doc, out);
  return out;
}

static std::string tr(const char *s, size_t cap = 128) {
  char out[256];
  translit(s, out, cap < sizeof(out) ? cap : sizeof(out));
  return out;
}

static void testState() {
  EQS(parse(L_STATE, kState), "");
  CHECK(g_np.have && g_np.ts == 1789412345UL && g_np.st == ST_PLAYING && g_np.kind == K_MUSIC);
  EQS(g_np.player, "media_player.nickoscope32_audio_s3");
  EQS(g_np.src, "MA");
  EQS(g_np.title, "Olive Tree");
  CHECK(g_np.vol == 50 && !g_np.muted && g_np.pos == 56 && g_np.posAt == 1789412300UL && g_np.dur == 241);

  // header
  EQS(parse(L_STATE, stateWith("v", nullptr)), "v");
  EQS(parse(L_STATE, stateWith("v", "2")), "v");
  EQS(parse(L_STATE, stateWith("v", "true")), "v");
  EQS(parse(L_STATE, stateWith("ts", nullptr)), "ts");
  EQS(parse(L_STATE, stateWith("ts", "\"1789412345\"")), "ts");
  EQS(parse(L_STATE, stateWith("ts", "5")), "ts");
  EQS(parse(L_STATE, stateWith("ts", "-1789412345")), "ts");
  EQS(parse(L_STATE, "[1,2]"), "not an object");

  // vocabulary
  EQS(parse(L_STATE, stateWith("st", "\"buffering\"")), "st");
  EQS(parse(L_STATE, stateWith("st", nullptr)), "st");
  EQS(parse(L_STATE, stateWith("kind", "\"video\"")), "kind");
  EQS(parse(L_STATE, stateWith("kind", nullptr)), "");
  CHECK(g_np.kind == K_MUSIC);
  for (int s = 0; s < ST_COUNT; s++) {
    const std::string raw = std::string("\"") + kStateKeys[s] + "\"";
    EQS(parse(L_STATE, stateWith("st", raw.c_str())), "");
    CHECK(g_np.st == s);
  }
  // no player: no state needed, and none is claimed
  EQS(parse(L_STATE, "{\"v\":1,\"ts\":1789412345,\"player\":\"\"}"), "");
  CHECK(g_np.have && !g_np.player[0] && g_np.st == ST_UNAVAILABLE && g_np.vol == -1);

  // ids
  EQS(parse(L_STATE, stateWith("player", "\"light.kitchen\"")), "player");
  EQS(parse(L_STATE, stateWith("player", "\"media_player.Kitchen\"")), "player");
  EQS(parse(L_STATE, stateWith("player", "\"media_player.\"")), "player");
  EQS(parse(L_STATE, stateWith("player", "42")), "player");
  std::string id63 = "\"media_player." + std::string(63 - 13, 'a') + "\"";
  std::string id64 = "\"media_player." + std::string(64 - 13, 'a') + "\"";
  EQS(parse(L_STATE, stateWith("player", id63.c_str())), "");
  EQS(parse(L_STATE, stateWith("player", id64.c_str())), "player");

  // numbers
  EQS(parse(L_STATE, stateWith("vol", "101")), "vol");
  EQS(parse(L_STATE, stateWith("vol", "-1")), "vol");
  EQS(parse(L_STATE, stateWith("vol", "\"50\"")), "vol");
  EQS(parse(L_STATE, stateWith("vol", "true")), "vol");
  EQS(parse(L_STATE, stateWith("vol", "49.5")), "vol");
  EQS(parse(L_STATE, stateWith("vol", "null")), "");
  CHECK(g_np.vol == -1);
  EQS(parse(L_STATE, stateWith("vol", "0")), "");
  CHECK(g_np.vol == 0);
  EQS(parse(L_STATE, stateWith("muted", "\"yes\"")), "muted");
  EQS(parse(L_STATE, stateWith("muted", "true")), "");
  CHECK(g_np.muted);
  EQS(parse(L_STATE, stateWith("pos", "1.5")), "pos");
  EQS(parse(L_STATE, stateWith("pos", "-3")), "pos");
  EQS(parse(L_STATE, stateWith("dur", "604801")), "dur");
  EQS(parse(L_STATE, stateWith("dur", "604800")), "");
  EQS(parse(L_STATE, stateWith("pos_at", "\"2026-09-14T19:47:53Z\"")), "pos_at");

  // strings: type, tags, bounds
  EQS(parse(L_STATE, stateWith("title", "12")), "title");
  EQS(parse(L_STATE, stateWith("artist", "[]")), "artist");
  EQS(parse(L_STATE, stateWith("src", "\"ma\"")), "src");
  EQS(parse(L_STATE, stateWith("src", "\"MUSIC\"")), "src");
  EQS(parse(L_STATE, stateWith("src", nullptr)), "");
  EQS(g_np.src, "HA");
  EQS(parse(L_STATE, stateWith("err", "\"BAD-X\"")), "err");
  EQS(parse(L_STATE, stateWith("err", "\"NOT_MA\"")), "");
  EQS(g_np.err, "NOT_MA");
  EQS(parse(L_STATE, stateWith("name", nullptr)), "");
  EQS(g_np.name, "nickoscope32_audio_s3");

  std::string ya;
  for (int i = 0; i < 300; i++) ya += "\xD0\xAF";   // Я
  std::string raw = "\"" + ya + "\"";
  EQS(parse(L_STATE, stateWith("title", raw.c_str())), "");
  CHECK(std::strlen(g_np.title) == 126);          // 63 whole letters in 127 bytes
  EQS(parse(L_STATE, stateWith("album", "\"a\\u0007b\\tc\"")), "");
  EQS(g_np.album, "a b c");
  // \u escapes arrive as UTF-8, and the page draws them transliterated
  EQS(parse(L_STATE, stateWith("title", "\"\\u0424\\u0435\\u0440\\u043c\\u0430\"")), "");
  EQS(tr(g_np.title), "Ferma");

  // a failed parse is reported whatever *out holds; the caller keeps its live copy
  EQS(parse(L_STATE, stateWith("vol", "300")), "vol");
}

static void testCopy() {
  char b[16];
  CHECK(copyUtf8(b, 5, "\xD0\xAF\xD0\xAF\xD0\xAF") == 4);
  EQS(b, "\xD0\xAF\xD0\xAF");
  copyUtf8(b, sizeof(b), "a\tb\x7f");
  EQS(b, "a b ");
  copyUtf8(b, sizeof(b), "\xC3(");
  EQS(b, "?(");
  copyUtf8(b, sizeof(b), "\xE2\x82");            // cut short
  EQS(b, "??");
  CHECK(copyUtf8(b, 1, "abc") == 0 && b[0] == '\0');
  copyUtf8(b, sizeof(b), nullptr);
  EQS(b, "");
}

static void testLists() {
  std::string many = "{\"v\":1,\"ts\":1789412345,\"sel\":\"media_player.p0\",\"list\":[";
  for (int i = 0; i < 10; i++) many += std::string(i ? "," : "") + "{\"id\":\"media_player.p" + std::to_string(i) + "\",\"name\":\"P" + std::to_string(i) + "\",\"st\":\"idle\"}";
  many += "]}";
  EQS(parse(L_PLAYERS, many), "");
  CHECK(g_pl.n == kMaxPlayers && g_pl.dropped == 2);
  EQS(g_pl.sel, "media_player.p0");

  EQS(parse(L_PLAYERS,
            "{\"v\":1,\"ts\":1789412345,\"list\":[{\"id\":\"light.x\"},{\"name\":\"no id\"},{\"id\":\"media_player.abc\"},"
            "{\"id\":\"media_player.abc\",\"name\":\"again\"},{\"id\":\"media_player.b\",\"st\":\"buffering\"},"
            "{\"id\":\"media_player.c\",\"name\":7},\"text\",{\"id\":\"media_player.yandex_station_u00c533009fayb\","
            "\"name\":\"\\u042f\\u043d\\u0434\\u0435\\u043a\\u0441 \\u0421\\u0442\\u0430\\u043d\\u0446\\u0438\\u044f 2\",\"st\":\"paused\"}]}"),
      "");
  CHECK(g_pl.n == 2 && g_pl.dropped == 6);
  EQS(g_pl.p[0].name, "abc");
  CHECK(g_pl.p[0].st == ST_UNAVAILABLE && g_pl.p[1].st == ST_PAUSED);
  EQS(tr(g_pl.p[1].name), "Iandeks Stantsiia 2");
  EQS(parse(L_PLAYERS, "{\"v\":1,\"ts\":1789412345,\"sel\":\"x\",\"list\":[]}"), "sel");
  EQS(parse(L_PLAYERS, "{\"v\":1,\"ts\":1789412345,\"list\":{}}"), "list");
  EQS(parse(L_PLAYERS, "{\"v\":1,\"ts\":1789412345}"), "list");

  std::string favs = "{\"v\":1,\"ts\":1789412345,\"list\":[";
  for (int i = 0; i < 20; i++) favs += std::string(i ? "," : "") + "{\"id\":\"library://radio/" + std::to_string(i) + "\",\"name\":\"R" + std::to_string(i) + "\"}";
  favs += ",{\"id\":\"a b\"}]}";
  EQS(parse(L_FAVS, favs), "");
  CHECK(g_fv.n == kMaxFavs && g_fv.dropped == 5);
  EQS(g_fv.f[3].id, "library://radio/3");
  EQS(parse(L_FAVS, "{\"v\":1,\"ts\":1789412345,\"list\":[{\"id\":\"q\\\"uote\"},{\"id\":\"back\\\\slash\"},{\"id\":\"\"},{\"id\":\"library://radio/2\"}]}"), "");
  CHECK(g_fv.n == 1 && g_fv.dropped == 3);
  EQS(g_fv.f[0].name, "library://radio/2");

  CHECK(validFavId(std::string(95, 'x').c_str()));
  CHECK(!validFavId(std::string(96, 'x').c_str()));
  CHECK(!validFavId("caf\xC3\xA9"));
  CHECK(validPlayerId("media_player.eversolo_dmp_a8"));
  CHECK(!validPlayerId("media_player.eversolo-dmp"));
  CHECK(!validPlayerId(nullptr));
}

static void testTranslit() {
  EQS(tr("\xD0\xA4\xD0\xB5\xD1\x80\xD0\xBC\xD0\xB0 \xD0\x9A\xD0\xBB\xD0\xB0\xD1\x80\xD0\xBA\xD1\x81\xD0\xBE\xD0\xBD\xD0\xB0 (s1e3)"),
      "Ferma Klarksona (s1e3)");                                           // Ферма Кларксона (s1e3)
  EQS(tr("\xD0\xA9\xD0\xB5\xD0\xB4\xD1\x80\xD0\xB8\xD0\xBD"), "Shchedrin"); // Щедрин
  EQS(tr("\xD0\xA9\xD0\x98"), "SHCHI");                                     // ЩИ
  EQS(tr("\xD0\xA9\xD0\xB8"), "Shchi");                                     // Щи
  EQS(tr("\xD0\x96"), "Zh");                                                // Ж
  EQS(tr("\xD0\x96\xD0\x96"), "ZHZH");                                      // ЖЖ
  EQS(tr("\xD0\x92\xD0\x98\xD0\xA7"), "VICH");                              // ВИЧ
  EQS(tr("\xD0\xA2\xD0\x90\xD0\x9D\xD0\xA6\xD0\xAB"), "TANTSY");            // ТАНЦЫ
  EQS(tr("\xD0\x9B\xD1\x8E\xD0\xB1\xD0\xBE\xD0\xB2\xD1\x8C"), "Liubov");    // Любовь: the soft sign is not in the table
  EQS(tr("\xD0\xAE\xD1\x80\xD0\xB8\xD0\xB9"), "Iurii");                     // Юрий
  EQS(tr("\xD0\x81\xD0\xBB\xD0\xBA\xD0\xB0"), "Elka");                      // Ёлка
  EQS(tr("\xD0\xA1\xD1\x8A\xD0\xB5\xD0\xB7\xD0\xB4"), "Sieezd");            // Съезд: Ъ is IE
  EQS(tr("\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE \xE2\x80\x94 \xD0\x93\xD1\x80\xD1\x83\xD0\xBF\xD0\xBF\xD0\xB0 \xD0\xBA\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB8"),
      "Kino - Gruppa krovi");                                              // Кино — Группа крови
  EQS(tr("\xD0\x9A\xD0\xB8\xD1\x97\xD0\xB2"), "Kiiv");                      // Київ: no per-language exception
  EQS(tr("\xD2\x90\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xBA"), "Ganok");             // Ґанок
  EQS(tr("\xD0\x9E\xD0\x9E\xD0\x9E \xC2\xAB\xD0\xA0\xD0\xBE\xD0\xBC\xD0\xB0\xD1\x88\xD0\xBA\xD0\xB0\xC2\xBB"),
      "OOO \"Romashka\"");                                                 // ООО «Ромашка»
  EQS(tr("Bj\xC3\xB6rk \xE2\x80\x93 J\xC3\xB3ga"), "Bjork - Joga");
  EQS(tr("Stra\xC3\x9F" "e"), "Strasse");
  EQS(tr("\xC5\x92uvre"), "Oeuvre");
  EQS(tr("\xC3\x86THER"), "AETHER");
  EQS(tr("\xF0\x9F\x94\xA5 Hot Hits \xF0\x9F\x8E\xB6"), "Hot Hits");       // emoji dropped, ends trimmed
  EQS(tr("\xE2\x9D\xA4\xEF\xB8\x8F love"), "love");                         // heart + variation selector
  EQS(tr("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"), "?");                     // 日本語: one '?' for the run
  EQS(tr("A\xE2\x80\x8B" "B"), "AB");                                       // zero width space
  EQS(tr("\xE2\x80\x9CQuoted\xE2\x80\x9D \xE2\x80\x98single\xE2\x80\x99 \xE2\x80\xA6"), "\"Quoted\" 'single' ...");
  EQS(tr("e\xCC\x81"), "e");                                                // combining acute
  EQS(tr("\xFF" "abc"), "?abc");                                            // broken byte
  EQS(tr("a   b\t\tc "), "a b c");
  EQS(tr("\xE2\x84\x96" "5"), "No5");
  EQS(tr(""), "");
  EQS(tr(nullptr), "");
  EQS(tr("\xD0\xA9\xD0\xB5\xD0\xB4\xD1\x80\xD0\xB8\xD0\xBD \xD0\xA9", 8), "Shchedr");   // bounded, terminated
  // Only what the fonts draw comes out, whatever goes in.
  std::string all;
  for (int c = 1; c < 256; c++) all += (char)c;
  for (unsigned cp = 0x80; cp < 0x800; cp++) { all += (char)(0xC0 | (cp >> 6)); all += (char)(0x80 | (cp & 0x3F)); }
  char out[4096];
  translit(all.c_str(), out, sizeof(out));
  bool printable = true;
  for (const char *p = out; *p; p++) printable = printable && (unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x7E;
  CHECK(printable);
}

static void testTrack() {
  NowPlaying a, b;
  EQS(parse(L_STATE, kState), "");
  a = g_np;
  // Home Assistant's keepalive: a newer ts, a new anchor, another volume. The same track.
  EQS(parse(L_STATE, stateWith("ts", "1789412405")), "");
  b = g_np;
  b.pos = 116; b.posAt = 1789412405UL; b.vol = 60; b.st = ST_PAUSED;
  CHECK(trackKey(a) == trackKey(b));
  std::strcpy(b.title, "Another");
  CHECK(trackKey(a) != trackKey(b));
  b = a; std::strcpy(b.player, "media_player.eversolo_dmp_a8");
  CHECK(trackKey(a) != trackKey(b));
  b = a; std::strcpy(b.title, "ab"); std::strcpy(b.artist, "c");
  NowPlaying c = a; std::strcpy(c.title, "a"); std::strcpy(c.artist, "bc");
  CHECK(trackKey(b) != trackKey(c));

  NowPlaying p = a;   // playing, pos 56 at 1789412300, dur 241
  CHECK(positionAt(p, 1789412300LL) == 56);
  CHECK(positionAt(p, 1789412330LL) == 86);
  CHECK(positionAt(p, 1789419999LL) == 241);         // never past the end
  CHECK(positionAt(p, 1789412000LL) == 56);          // a clock behind the anchor
  p.st = ST_PAUSED;
  CHECK(positionAt(p, 1789412330LL) == 56);
  p.st = ST_PLAYING; p.posAt = 0;
  CHECK(positionAt(p, 1789412330LL) == 56);
  p = a; p.dur = 0; p.pos = 0;                       // radio
  CHECK(positionAt(p, 1789412300LL + 5000000LL) == kMaxSeconds);

  char t[16];
  formatClock(t, sizeof(t), 0);    EQS(t, "0:00");
  formatClock(t, sizeof(t), 61);   EQS(t, "1:01");
  formatClock(t, sizeof(t), 241);  EQS(t, "4:01");
  formatClock(t, sizeof(t), 3600); EQS(t, "1:00:00");
  formatClock(t, sizeof(t), 3725); EQS(t, "1:02:05");
}

static std::string field(const char *json, const char *key) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "<bad json>";
  std::string out;
  serializeJson(doc[key], out);
  return out;
}

static void testCommands() {
  char b[256];
  const char *p = "media_player.nickoscope32_audio_s3";
  CHECK(cmdSimple(b, sizeof(b), "toggle", p) > 0);
  EQS(field(b, "cmd"), "\"toggle\"");
  EQS(field(b, "player"), "\"media_player.nickoscope32_audio_s3\"");
  EQS(field(b, "v"), "1");
  CHECK(cmdSimple(b, sizeof(b), "stop", p) == 0);
  CHECK(cmdSimple(b, sizeof(b), "next", "media_player.\"x") == 0);
  CHECK(cmdSimple(b, 20, "next", p) == 0);             // does not fit
  CHECK(cmdInt(b, sizeof(b), "vol", p, 42) > 0);
  EQS(field(b, "value"), "42");
  CHECK(cmdInt(b, sizeof(b), "vol", p, 101) == 0);
  CHECK(cmdInt(b, sizeof(b), "vol_step", p, -4) > 0);
  EQS(field(b, "value"), "-4");
  CHECK(cmdInt(b, sizeof(b), "vol_step", p, 0) == 0);
  CHECK(cmdInt(b, sizeof(b), "vol_step", p, 21) == 0);
  CHECK(cmdInt(b, sizeof(b), "seek", p, 1) == 0);
  CHECK(cmdMute(b, sizeof(b), p, true) > 0);
  EQS(field(b, "value"), "true");
  CHECK(cmdFav(b, sizeof(b), p, "library://radio/2") > 0);
  EQS(field(b, "id"), "\"library://radio/2\"");
  EQS(field(b, "cmd"), "\"play_fav\"");
  CHECK(cmdFav(b, sizeof(b), p, "bad\"id") == 0);
  CHECK(selectPayload(b, sizeof(b), p) > 0);
  EQS(field(b, "player"), "\"media_player.nickoscope32_audio_s3\"");
  CHECK(selectPayload(b, sizeof(b), "") == 0);
  // the longest command there can be fits the firmware's buffer for it (media_ha.cpp: 224)
  std::string longId = "media_player." + std::string(50, 'z'), longFav(95, 'f');
  CHECK(cmdFav(b, 224, longId.c_str(), longFav.c_str()) > 0);
}

// The biggest payloads the app may send (kPayloadMax) parse well inside the
// firmware's JSON cap (media_ha.cpp JSON_CAP = 12288).
static void testSizes() {
  static const char *kCyr16 = "\xD0\xA0\xD0\xB0\xD0\xB4\xD0\xB8\xD0\xBE \xD0\x9C\xD0\xB0\xD1\x8F\xD0\xBA 1";  // Радио Маяк 1
  std::string favs = "{\"v\":1,\"ts\":1789412345,\"list\":[";
  for (int i = 0; i < 16; i++) {
    std::string item = std::string(i ? "," : "") + "{\"id\":\"radiobrowser://radio/9617a958-0601-11e8-ae97-5254" + std::to_string(1000 + i) +
                       "\",\"name\":\"" + kCyr16 + std::to_string(i) + "\"}";
    if (favs.size() + item.size() + 2 > kPayloadMax) break;
    favs += item;
  }
  favs += "]}";
  size_t peak = 0;
  EQS(parse(L_FAVS, favs, &peak), "");
  std::printf("  favs    %4zu B payload, %2u stations -> parse peak %5zu B\n", favs.size(), (unsigned)g_fv.n, peak);
  CHECK(favs.size() <= kPayloadMax && peak < 12288);

  std::string big = std::string(kCyr16);
  while (big.size() < 200) big += kCyr16;
  JsonDocument doc;
  deserializeJson(doc, kState);
  doc["title"] = big; doc["artist"] = big; doc["album"] = big; doc["name"] = big; doc["err"] = "CALL_FAILED";
  std::string state;
  serializeJson(doc, state);
  EQS(parse(L_STATE, state, &peak), "");
  std::printf("  state   %4zu B payload                -> parse peak %5zu B\n", state.size(), peak);
  CHECK(state.size() <= kPayloadMax && peak < 12288);

  std::string players = "{\"v\":1,\"ts\":1789412345,\"sel\":\"media_player.p0\",\"list\":[";
  for (int i = 0; i < 8; i++)
    players += std::string(i ? "," : "") + "{\"id\":\"media_player.yandex_station_u00c53300" + std::to_string(1000 + i) + "\",\"name\":\"" +
               kCyr16 + kCyr16 + "\",\"st\":\"unavailable\"}";
  players += "]}";
  EQS(parse(L_PLAYERS, players, &peak), "");
  std::printf("  players %4zu B payload, %u players     -> parse peak %5zu B\n", players.size(), (unsigned)g_pl.n, peak);
  CHECK(players.size() <= kPayloadMax && peak < 12288);
  std::printf("  model   NowPlaying %zu B, Players %zu B, Favs %zu B\n", sizeof(NowPlaying), sizeof(Players), sizeof(Favs));
}

static int probeTranslit() {
  std::string line;
  char out[512];
  while (std::getline(std::cin, line)) {
    translit(line.c_str(), out, sizeof(out));
    std::printf("%s\n", out);
  }
  return 0;
}

static int probeParse(const char *leafName, const char *path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::printf("REFUSED cannot open\n"); return 2; }
  std::stringstream ss;
  ss << f.rdbuf();
  const Leaf leaf = !std::strcmp(leafName, "state") ? L_STATE : (!std::strcmp(leafName, "players") ? L_PLAYERS : L_FAVS);
  size_t peak = 0;
  const std::string why = parse(leaf, ss.str(), &peak);
  if (why.empty()) { std::printf("OK %zu B peak %zu B\n", ss.str().size(), peak); return 0; }
  std::printf("REFUSED %s\n", why.c_str());
  return 1;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "translit")) return probeTranslit();
  if (argc >= 4 && !std::strcmp(argv[1], "parse")) return probeParse(argv[2], argv[3]);
  testState();
  testCopy();
  testLists();
  testTranslit();
  testTrack();
  testCommands();
  testSizes();
  std::printf("  media_model: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
