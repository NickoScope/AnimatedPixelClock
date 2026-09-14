#pragma once
// Media player: the data model, its validation and the text it draws.
//
// Plain C++ and ArduinoJson, no Arduino core, so tools/media/check_media.py
// compiles it on the host: payload parsing and bounds, transliteration,
// progress extrapolation, the track key and the command payloads are tested
// there against the same code the panel runs.
//
// The wire contract (topics, every field, who publishes what) is in
// src/media/README.md. What this file enforces:
//
//   * schema "v":1 and an epoch "ts" on every payload from Home Assistant;
//   * every field of the right JSON type, or the whole payload is refused and
//     what the page shows stays as it was;
//   * every string cut to its buffer at a whole UTF-8 code point, control
//     characters turned into spaces, broken sequences into '?';
//   * ids from a closed alphabet: a player is media_player.<a-z 0-9 _>, a
//     favourite is printable ASCII without quotes or backslashes;
//   * at most kMaxPlayers players and kMaxFavs favourites; the rest dropped
//     and counted.

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

namespace media {

static const long kSchema = 1;

// Buffer sizes in bytes, with the terminating NUL. A player id longer than 63
// is refused (Home Assistant allows 255; none on the owner's HA is over 40).
static const size_t   kPlayerIdLen = 64;
static const size_t   kFavIdLen    = 96;    // MA URIs: library://radio/18, radiobrowser://radio/<uuid>
static const size_t   kNameLen     = 48;
static const size_t   kTitleLen    = 128;
static const size_t   kArtistLen   = 96;
static const size_t   kAlbumLen    = 96;
static const size_t   kSrcLen      = 5;     // a tag of up to four capitals or digits: MA, HA, YA
static const size_t   kErrLen      = 17;    // up to sixteen of A-Z 0-9 _
static const uint8_t  kMaxPlayers  = 8;
static const uint8_t  kMaxFavs     = 16;
static const uint32_t kMaxSeconds  = 604800;   // position and duration: a week is not a track
// src/mqtt/mqtt_bus.cpp's buffer holds the fixed header and the topic as well
// as the payload, so a payload must stay this far under it.
static const size_t   kBusBuffer   = 2048;
static const size_t   kPayloadMax  = 1900;

// The page's vocabulary, in this order on both ends.
enum State : uint8_t { ST_UNAVAILABLE = 0, ST_OFF, ST_IDLE, ST_PAUSED, ST_PLAYING, ST_COUNT };
enum Kind : uint8_t { K_MUSIC = 0, K_RADIO, K_TTS, K_COUNT };
extern const char *const kStateKeys[ST_COUNT];   // "unavailable" "off" "idle" "paused" "playing"
extern const char *const kKindKeys[K_COUNT];     // "music" "radio" "tts"

struct NowPlaying {
  bool     have;
  uint32_t ts;                 // when Home Assistant last confirmed this, UTC epoch seconds
  char     player[kPlayerIdLen];   // "" = Home Assistant follows no player
  char     name[kNameLen];
  char     src[kSrcLen];
  char     err[kErrLen];       // "" or the last command's failure, e.g. NOT_MA
  uint8_t  st;                 // State
  uint8_t  kind;               // Kind
  char     title[kTitleLen];
  char     artist[kArtistLen];
  char     album[kAlbumLen];
  int8_t   vol;                // 0..100, -1 not reported
  bool     muted;
  uint32_t pos;                // seconds into the track at posAt
  uint32_t posAt;              // UTC epoch seconds of that anchor, 0 = none
  uint32_t dur;                // seconds, 0 = none (radio)
};

struct Player {
  char    id[kPlayerIdLen];
  char    name[kNameLen];
  uint8_t st;
};

struct Players {
  bool     have;
  uint32_t ts;
  uint8_t  n;
  uint8_t  dropped;            // entries past the cap or malformed
  char     sel[kPlayerIdLen];  // the player Home Assistant follows
  Player   p[kMaxPlayers];
};

struct Fav {
  char id[kFavIdLen];
  char name[kNameLen];
};

struct Favs {
  bool     have;
  uint32_t ts;
  uint8_t  n;
  uint8_t  dropped;
  Fav      f[kMaxFavs];
};

// Each returns nullptr with *out filled, or a short reason (a literal). *out is
// scratch: on a reason its content is undefined, so parse into a spare and
// copy it over the live one only on success - media_ha.cpp does, which is
// what keeps a bad payload off the screen.
const char *stateFrom(JsonObjectConst in, NowPlaying *out);
const char *playersFrom(JsonObjectConst in, Players *out);
const char *favsFrom(JsonObjectConst in, Favs *out);

bool validPlayerId(const char *s);
bool validFavId(const char *s);

// Bounded copy of UTF-8: whole code points only, control characters become a
// space, a broken sequence becomes '?'. Returns the bytes written.
size_t copyUtf8(char *dst, size_t cap, const char *src);

// UTF-8 to what the panel's fonts can draw (0x20..0x7E). Cyrillic follows the
// ICAO Doc 9303 transliteration (media_model.cpp has the table and the
// source); Latin letters with diacritics lose them; typographic quotes, dashes
// and spaces become their ASCII forms; emoji, variation selectors and
// combining marks are dropped; anything else is one '?' per run. Runs of
// spaces become one and the ends are trimmed. Returns the length written.
size_t translit(const char *utf8, char *out, size_t cap);

// Identifies a track for "new track" effects: the player, title, artist and
// album. The same retained payload delivered again - after a reconnect, or as
// Home Assistant's keepalive with a newer ts - gives the same key.
uint32_t trackKey(const NowPlaying &np);

// Seconds into the track at `now`: the anchor plus the time since it while
// playing, the anchor itself otherwise, never past the duration. A clock
// behind the anchor gets the anchor.
uint32_t positionAt(const NowPlaying &np, int64_t now);

// "m:ss", or "h:mm:ss" from an hour.
void formatClock(char *out, size_t cap, uint32_t seconds);

// Command payloads, panel to Home Assistant. Each returns the length written,
// or 0 when it would not fit `cap` or an argument is not valid.
size_t cmdSimple(char *out, size_t cap, const char *cmd, const char *player);   // toggle play pause next prev
size_t cmdInt(char *out, size_t cap, const char *cmd, const char *player, long value);   // vol 0..100, vol_step -20..20
size_t cmdMute(char *out, size_t cap, const char *player, bool muted);
size_t cmdFav(char *out, size_t cap, const char *player, const char *favId);
size_t selectPayload(char *out, size_t cap, const char *player);                 // the retained select topic

}  // namespace media
