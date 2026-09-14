#!/usr/bin/env python3
"""Write the SYNTHETIC AeroAPI answers the flight board's host tests read.

Nothing here came from FlightAware. Every record is made up; the field names,
types and nesting follow the BaseFlight schema of the AeroAPI OpenAPI spec
4.17.1 (`/airports/{id}/flights/arrivals` 200 response, and `/flights/{ident}`)
and the shape of one record Home Assistant's REST sensor held on 2026-09-14,
which is not stored in this repository. Each record exists to exercise one
rule; the comment beside it says which.

  python3 tools/flightboard/gen_aero_fixture.py          # writes tools/flightboard/samples/
  python3 tools/flightboard/gen_aero_fixture.py --check  # exit 1 if the files differ
"""
import calendar
import datetime as dt
import json
import pathlib
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent
SAMPLES = HERE / "samples"
NOW = calendar.timegm((2026, 9, 14, 18, 0, 0))     # 20:00 in Nice


def iso(minutes):
    if minutes is None:
        return None
    return dt.datetime.fromtimestamp(NOW + minutes * 60, tz=dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def apt(icao, iata, city, name, tz):
    return {"code": icao, "code_icao": icao, "code_iata": iata, "code_lid": None, "timezone": tz,
            "name": name, "city": city, "airport_info_url": f"/airports/{icao}"}


NCE = apt("LFMN", "NCE", "Nice", "Nice Côte d'Azur", "Europe/Paris")
LHR = apt("EGLL", "LHR", "London", "London Heathrow", "Europe/London")
CDG = apt("LFPG", "CDG", "Paris", "Paris Charles de Gaulle", "Europe/Paris")
ORY = apt("LFPO", "ORY", "Paris (Orly)", "Paris Orly", "Europe/Paris")
ZRH = apt("LSZH", "ZRH", "Zürich", "Zurich", "Europe/Zurich")
FRA = apt("EDDF", "FRA", "Frankfurt am Main", "Frankfurt Int'l", "Europe/Berlin")
BCN = apt("LEBL", "BCN", "Barcelona", "Barcelona El Prat", "Europe/Madrid")
JFK = apt("KJFK", "JFK", "New York", "John F Kennedy Intl", "America/New_York")
LTZ = apt("LFTZ", None, "La Mole (Saint-Tropez)", "La Mole", "Europe/Paris")
OSL = apt("ENGM", "OSL", "Oslo/Gardermoen", "Oslo Gardermoen", "Europe/Oslo")
VIE = apt("LOWW", "VIE", "Wien", "Vienna Int'l", "Europe/Vienna")
AMS = apt("EHAM", "AMS", "Amsterdam", "Amsterdam Schiphol", "Europe/Amsterdam")
KRK = apt("EPKK", "KRK", "Kraków", "Krakow John Paul II", "Europe/Warsaw")


def flight(icao, iata, origin, dest, fid=True, so=None, eo=None, ao=None, off=None, on=None,
           si=None, ei=None, ai=None, cancelled=False, diverted=False, dep_delay=None, arr_delay=None,
           status="Scheduled", gate=None, eon=None):
    ident = icao or iata or ""
    return {
        "ident": ident, "ident_icao": icao, "ident_iata": iata,
        "actual_runway_off": None, "actual_runway_on": None,
        # crc32, not hash(): Python salts str hashes per run, and the file must come out the same.
        "fa_flight_id": f"{ident}-1789300000-schedule-{zlib.crc32(repr((ident, so, si)).encode()) % 10000:04d}p"
        if fid is True else fid,
        "operator": (icao or "")[:3] or None, "operator_icao": (icao or "")[:3] or None, "operator_iata": (iata or "")[:2] or None,
        "flight_number": "".join(c for c in ident if c.isdigit()) or None,
        "registration": None, "atc_ident": None, "inbound_fa_flight_id": None,
        "codeshares": [], "codeshares_iata": [], "blocked": False,
        "diverted": diverted, "cancelled": cancelled, "position_only": False,
        "origin": origin, "destination": dest,
        "departure_delay": dep_delay, "arrival_delay": arr_delay, "filed_ete": 5400,
        "scheduled_out": iso(so), "estimated_out": iso(eo), "actual_out": iso(ao),
        "scheduled_off": iso(None if so is None else so + 10), "estimated_off": iso(None if eo is None else eo + 10),
        "actual_off": iso(off),
        "scheduled_on": iso(None if si is None else si - 8), "estimated_on": iso(eon if eon is not None else (None if ei is None else ei - 8)),
        "actual_on": iso(on),
        "scheduled_in": iso(si), "estimated_in": iso(ei), "actual_in": iso(ai),
        "progress_percent": 100 if ai or on else (50 if off else 0), "status": status,
        "aircraft_type": "A320", "route_distance": 400, "filed_airspeed": 440, "filed_altitude": None,
        "route": None, "baggage_claim": None, "seats_cabin_business": None, "seats_cabin_coach": None,
        "seats_cabin_first": None, "gate_origin": gate, "gate_destination": None,
        "terminal_origin": None, "terminal_destination": None, "type": "Airline",
    }


def page(key, flights, more=False):
    return {"links": {"next": f"/airports/LFMN/flights/{key}?cursor=synthetic"} if more else None,
            "num_pages": 1, key: flights}


def lists():
    dlh = dict(fid="DLH1234-1789300000-schedule-0001p")
    arrivals = [   # ordered by actual_on descending, as the spec says
        flight("DLH1234", "LH1234", FRA, NCE, **dlh, so=-110, ao=-108, off=-100, on=-14, si=-15, ei=-12, ai=-10,
               status="Arrived / Gate Arrival"),                                   # in both lists: this one wins
        flight("BAW342", "BA342", LHR, NCE, so=-160, ao=-158, off=-150, on=-45, si=-40, ei=-40, ai=-38),  # landed
        flight("EZY8012", "U28012", CDG, NCE, so=-180, off=-170, on=-100, si=-98, ai=-95),                 # landed
        flight("IBE3456", "IB3456", BCN, NCE, so=-90, off=-80, on=-6, si=0, ei=-2,
               status="Landed / Taxiing"),                                         # runway, not gate: land
        flight("SWR560", "LX560", ZRH, NCE, so=-220, off=-210, on=-133, si=-132, ai=-130),                 # outside -2 h
        flight(None, None, VIE, NCE, fid=None, si=-20, ai=-18),                            # no ident at all: dropped
        flight("RYR99", "FR99", BCN, NCE),                                                 # no gate time: dropped
    ]
    scheduled_arrivals = [
        flight("DLH1234", "LH1234", FRA, NCE, **dlh, so=-110, off=-100, si=-15, ei=-8),   # stale copy: dup
        flight("AFR7302", "AF7302", CDG, NCE, so=-60, ao=-58, off=-50, si=20, ei=22),     # en route: dep
        flight("VLG1500", "VY1500", BCN, NCE, so=-20, eo=5, si=35, ei=60),                 # 25 min late: delay
        flight("DAL82", "DL82", JFK, NCE, so=-480, ao=-470, off=-460, si=90, ei=85),       # en route
        flight("MYJ12", None, LTZ, NCE, si=100, ei=100),                                   # no IATA anywhere
        flight("SAS4371", "SK4371", OSL, NCE, so=60, si=200, ei=200),                      # outside +2 h
        flight("EJU4410", "U24410", VIE, NCE, so=-60, si=45, ei=45, cancelled=True),       # cancelled
        flight("KLM1263", "KL1263", AMS, NCE, so=-10, ao=-5, si=110, ei=110),              # left the gate: board
        flight("LOT415", "LO415", KRK, NCE, so=0, si=120, ei=120),                         # exactly +2 h: kept
    ]
    departures = [   # ordered by actual_off descending
        flight("AFR7301", "AF7301", NCE, CDG, so=-30, ao=-28, off=-15, si=55, ei=57),
        flight(f"EZY8013", "U28013", NCE, ORY, so=-125, ao=-120, off=-105, si=-20, ei=-22, ai=-21),  # t exactly -2 h
        flight("BAW345", "BA345", NCE, LHR, so=-130, ao=-121, off=-110),                            # t -121: outside
        flight("NOID1", "NO1", NCE, ZRH, fid=None, so=-60, ao=-58, off=-50),               # no fa_flight_id
    ] + [flight(f"EZY{8100 + i}", f"U2{8100 + i}", NCE, LHR, so=-100 + 11 * i, ao=-99 + 11 * i, off=-90 + 11 * i)
         for i in range(8)]
    scheduled_departures = [
        flight("NOID1", "NO1", NCE, ZRH, fid=None, so=-60, eo=-60),                        # the same, again: dup
        flight("BAW343", "BA343", NCE, LHR, so=15, eo=15, gate="12"),
        flight("SWR561", "LX561", NCE, ZRH, so=40, eo=70),                                 # delay
    ] + [flight(f"TVF{3000 + i}", f"TO{3000 + i}", NCE, ORY, so=20 + 7 * i, eo=20 + 7 * i) for i in range(13)]
    return {
        "arrivals": page("arrivals", arrivals, more=True),
        "scheduled_arrivals": page("scheduled_arrivals", scheduled_arrivals),
        "departures": page("departures", departures),
        "scheduled_departures": page("scheduled_departures", scheduled_departures),
    }


H = 60   # minutes in an hour, for the tracker times below


def tracks():
    """Answers to GET /flights/AFR7301?ident_type=designator, and what the pick must make of them."""
    tomorrow = flight("AFR7301", "AF7301", NCE, CDG, so=23 * H, eo=23 * H, si=24 * H + 35, ei=24 * H + 35)
    yesterday = flight("AFR7301", "AF7301", NCE, CDG, so=-24 * H, ao=-24 * H + 2, off=-24 * H + 15,
                       on=-23 * H + 20, si=-23 * H + 35, ai=-23 * H + 28)
    added = NOW - 2 * 3600
    out = {}

    def case(name, answer, added_at, **expect):
        out[name] = {"answer": {"links": None, "num_pages": 1, "flights": answer}, "added": added_at,
                     "expect": expect}

    enroute = flight("AFR7301", "AF7301", NCE, ORY, so=-40, eo=-35, ao=-33, off=-20, si=45, ei=52, eon=44,
                     arr_delay=420, dep_delay=300, gate="A7")
    case("enroute", [tomorrow, enroute, yesterday], added,
         state="ENROUTE", fn="AF7301", frm="NCE", to="ORY", current=True, flights=3,
         next_s=10 * 60, expires=0, shown=NOW + 52 * 60, delay_min=7)
    case("enroute_far", [dict(enroute, estimated_on=iso(90), estimated_in=iso(98))], added,
         state="ENROUTE", fn="AF7301", frm="NCE", to="ORY", current=True, flights=1,
         next_s=20 * 60, expires=0, shown=NOW + 98 * 60, delay_min=7)
    landed = flight("AFR7301", "AF7301", NCE, ORY, so=-120, ao=-118, off=-105, on=-20, si=-15, ai=-12, arr_delay=180)
    case("landed", [tomorrow, landed, yesterday], added,
         state="LANDED", fn="AF7301", frm="NCE", to="ORY", current=True, flights=3,
         next_s=0, expires=NOW - 12 * 60 + 2 * 3600, shown=NOW - 12 * 60, delay_min=3)
    # Landed 100 min ago, the tracker added now: more than 1 h before it was
    # added, so tomorrow's flight is the one meant.
    landed_early = dict(landed, actual_on=iso(-105), actual_in=iso(-100))
    case("landed_before_added", [tomorrow, landed_early], NOW,
         state="SCHED", fn="AF7301", frm="NCE", to="CDG", current=True, flights=2,
         next_s=6 * 3600, expires=0, shown=NOW + 23 * 3600, delay_min=0)
    case("ahead_6h", [dict(tomorrow, scheduled_out=iso(6 * H), estimated_out=iso(6 * H))], added,
         state="SCHED", fn="AF7301", frm="NCE", to="CDG", current=True, flights=1,
         next_s=(5 * 3600) // 3, expires=0, shown=NOW + 6 * 3600, delay_min=0)
    delayed = flight("AFR7301", "AF7301", NCE, CDG, so=30, eo=55, si=125, ei=150, dep_delay=1500, gate="B12")
    case("delayed", [tomorrow, delayed], added,
         state="DELAYED", fn="AF7301", frm="NCE", to="CDG", current=True, flights=2,
         next_s=10 * 60, expires=0, shown=NOW + 55 * 60, delay_min=25)
    taxi = flight("AFR7301", "AF7301", NCE, CDG, so=-5, eo=-3, ao=-2, si=90, ei=92)
    # No departure_delay: the delay is estimated_out less scheduled_out, 2 min.
    case("taxi", [taxi], added, state="TAXI", fn="AF7301", frm="NCE", to="CDG", current=True, flights=1,
         next_s=10 * 60, expires=0, shown=NOW - 2 * 60, delay_min=2)
    cancelled = flight("AFR7301", "AF7301", NCE, CDG, so=40, eo=40, si=135, cancelled=True, status="Cancelled")
    case("cancelled", [tomorrow, cancelled], added,
         state="CANCELLED", fn="AF7301", frm="NCE", to="CDG", current=True, flights=2,
         next_s=0, expires=NOW + 40 * 60 + 6 * 3600, shown=NOW + 40 * 60, delay_min=0)
    diverted = flight("AFR7301", "AF7301", NCE, ORY, so=-90, ao=-88, off=-75, si=-5, ei=20, diverted=True)
    case("diverted", [diverted], added, state="DIVERTED", fn="AF7301", frm="NCE", to="ORY", current=True,
         flights=1, next_s=30 * 60, expires=0, shown=NOW + 20 * 60, delay_min=0)
    case("only_old", [yesterday], added, state="LANDED", fn="AF7301", frm="NCE", to="CDG", current=False,
         flights=1, next_s=6 * 3600, expires=0, shown=NOW - 23 * 3600 + 28 * 60, delay_min=0)
    case("empty", [], added, state="NOTFOUND", fn="", frm="", to="", current=False, flights=0,
         next_s=6 * 3600, expires=0, shown=0, delay_min=0)
    return out


def files():
    out = {f"aero_{k}.json": v for k, v in lists().items()}
    out["aero_tracks.json"] = {"now": NOW, "cases": tracks()}
    return {k: json.dumps(v, indent=1, ensure_ascii=False) + "\n" for k, v in out.items()}


def main():
    want = files()
    if "--check" in sys.argv:
        stale = [k for k, v in want.items() if not (SAMPLES / k).is_file() or (SAMPLES / k).read_text() != v]
        for k in stale:
            print(f"stale: tools/flightboard/samples/{k}")
        sys.exit(1 if stale else 0)
    SAMPLES.mkdir(exist_ok=True)
    for k, v in want.items():
        (SAMPLES / k).write_text(v)
        print(f"wrote tools/flightboard/samples/{k}  {len(v):,} B")


if __name__ == "__main__":
    main()
