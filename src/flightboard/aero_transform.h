#pragma once
// FlightAware AeroAPI v4 -> the flight board, in C++.
//
// The board rules are a port of Home Assistant's automation "FLIGHT BOARD -
// obsluzhivanie zaprosa chasov (MQTT) v3.7" (id flightboard_serve), rule for
// rule, so the direct fetch fills exactly the rows the MQTT payload filled:
// merge the past and the future list, de-duplicate on fa_flight_id, keep
// +-2 h around now, sort by time, now_idx at the first flight not yet due,
// fifteen rows from seven before it, the same status mapping and city cleanup.
// Where this differs from the automation, on purpose, it says so below.
//
// Endpoints, parameters and fields: the AeroAPI OpenAPI spec, version 4.17.1
// (https://www.flightaware.com/commercial/aeroapi/resources/aeroapi-openapi.yml,
// read 2026-09-14). src/flightboard/README.md cites the lines.
//
// Plain C++ and ArduinoJson, no Arduino core: tools/flightboard/check_aero.py
// compiles and tests it on the host.

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>

#include "fb_model.h"

namespace aero {

// ── the board ───────────────────────────────────────────────────────────────
static const int64_t kWindowS = 7200;   // the automation's lo/hi: now -+ 7200 s
static const int64_t kDelayS  = 900;    // its "delay": estimated past scheduled by more than this
static const uint8_t kHalf    = 7;      // its "half": rows kept before now_idx
static const uint8_t kListMax = 20;     // candidates kept from one answer; one page is 15 records

// The four lists a board is made of, one AeroAPI call each.
enum List : uint8_t { ARR_PAST = 0, ARR_NEXT, DEP_PAST, DEP_NEXT, LIST_COUNT };
const char *listName(List l);           // the endpoint's last segment, which is also its array's key
inline bool listDeparts(List l) { return l == DEP_PAST || l == DEP_NEXT; }
inline bool listPast(List l)    { return l == ARR_PAST || l == DEP_PAST; }

// RFC 3339 date-time to UTC epoch seconds: "Z" or a +hh:mm zone, optional
// fractional seconds. A time with no zone is refused rather than guessed; the
// spec's times are format: date-time, and every one seen is "...Z".
bool parseTime(const char *s, int64_t *out);
// UTC epoch seconds to "YYYY-MM-DDTHH:MM:SSZ". `cap` must be at least 21.
void formatTime(int64_t utc, char *out, size_t cap);

// The path and query after https://aeroapi.flightaware.com/aeroapi, e.g.
// "/airports/LFMN/flights/scheduled_arrivals?type=Airline&max_pages=1&start=...".
// Without a clock (nowUtc 0) start and end are left out and AeroAPI's defaults
// apply. False when it does not fit.
bool listQuery(const char *icao, List l, int64_t nowUtc, char *out, size_t cap);

struct Cand {
  char     fn[FB_FN_LEN];
  char     ct[FB_CT_LEN];
  char     cy[FB_CY_LEN];
  uint8_t  st;          // FbStatus
  int64_t  t;           // UTC: actual, else estimated, else scheduled gate time
  uint32_t id;          // FNV-1a of fa_flight_id, or of ident|scheduled time without one
};

struct ListResult {
  Cand     c[kListMax];
  uint8_t  n;
  // What was left out, for a board that fetches and shows nothing.
  uint16_t seen;        // records in the answer
  uint16_t noTime;      // no gate time of any kind
  uint16_t noIdent;     // no ident_iata, ident_icao or ident
  uint16_t overflow;    // past kListMax
  bool     more;        // links.next was set: AeroAPI had more pages than max_pages=1 fetched
};

// Keeps only what parseList() reads.
void listFilter(JsonDocument &filter, List l);
// A 200 answer. False when it has no array under the list's key.
bool parseList(JsonVariantConst root, List l, ListResult *out);

struct Board {
  Cand     rows[FB_MAX_ROWS];
  uint8_t  count;
  uint8_t  nowIdx;      // may equal count when every flight is past, as in the automation
  uint16_t outside;     // candidates outside now -+ kWindowS
  uint16_t dup;         // candidates already taken from the other list
};

// One direction's board from its two lists, either of which may be null (not
// fetched yet). The past list is taken first, so a flight in both keeps the
// past list's record, as in the automation's (past + fut) loop.
void buildBoard(const ListResult *past, const ListResult *next, int64_t nowUtc, Board *out);

// ── tracked flights ─────────────────────────────────────────────────────────
// GET /flights/{ident}?ident_type=designator: without start and end AeroAPI
// returns about 14 days of flights under the ident; this asks for the day
// before and the two days after now (the spec allows 10 days back and 2 ahead).
bool trackQuery(const char *ident, int64_t nowUtc, char *out, size_t cap);

static const int64_t kNoTime = 0;

struct Track {
  uint8_t  state;              // FbTrackState
  char     fn[FB_FN_LEN];      // ident_iata, else ident_icao, else ident
  char     from[FB_CT_LEN];    // origin IATA, else ICAO
  char     to[FB_CT_LEN];
  char     gate[6];            // gate_origin, "" when unknown
  int64_t  schedOut, estOut, actOut, actOff;
  int64_t  schedIn, estIn, actIn, actOn, estOn;
  int32_t  depDelayS, arrDelayS;   // departure_delay / arrival_delay, 0 when null
  uint32_t id;
  uint8_t  flights;            // records in the answer
  bool     current;            // the pick found a flight in progress, recent or ahead
};

void trackFilter(JsonDocument &filter);

// Picks the flight a tracker means from a /flights/{ident} answer. False when
// the answer has no flights array (a bad answer). With the array present but no
// flight in it, state is FB_TRK_NOTFOUND.
//
// The pick, in order - a choice, not AeroAPI's:
//   1. one that has left the gate and not landed, not cancelled;
//   2. one that landed within 3 h, after the tracker was added (less 1 h);
//   3. the earliest one still on the ground whose departure is at most 6 h past;
//   4. otherwise the latest one, shown with current = false.
bool pickTrack(JsonVariantConst root, int64_t nowUtc, int64_t addedUtc, Track *out);

// Seconds until this tracker should be asked again; 0 = never (landed, cancelled).
//   not found              6 h
//   on the ground          a third of the time left until an hour before departure,
//                          between 10 min and 6 h: 24 h out 6 h, 6 h out 1 h 40,
//                          2 h out 20 min, the last hour and any delay 10 min
//   taxiing                10 min
//   airborne               20 min; 10 min once due within 45 min
//   diverted               30 min, until it lands somewhere
uint32_t trackNextS(const Track &t, int64_t nowUtc);

// When the tracker removes itself; 0 = it does not. 2 h after landing (the
// runway time if there is no gate time yet), 6 h after a cancelled flight's
// scheduled departure. Only for a flight picked as current.
int64_t trackExpiresAt(const Track &t);

// The one time the pinned row shows: the departure until the aircraft is off
// the ground, the arrival after. Actual, else estimated, else scheduled.
int64_t trackShownTime(const Track &t);
// Minutes late for the delay word: the departure's before take-off, the
// arrival's after. Negative when early.
int32_t trackDelayMin(const Track &t);

// ── text ────────────────────────────────────────────────────────────────────
// A place name from AeroAPI as the page draws it: cut at "(" and "/" as the
// automation does, then - unlike the automation, which only upper-cased it -
// Latin letters without their accents, capitals, and anything the font cannot
// draw as a space, collapsed and trimmed. "Zürich" is ZURICH, not a gap.
void cleanCity(const char *utf8, char *out, size_t cap);

// A flight ident as typed in the portal, folded to what AeroAPI is asked for:
// capitals, spaces removed. False unless it is 2 or 3 characters of airline
// code (letters and digits, at least one letter; ICAO codes are 3 letters,
// IATA codes 2 characters) then 1 to 4 digits and at most one letter.
bool normaliseIdent(const char *in, char *out, size_t cap);

}  // namespace aero
