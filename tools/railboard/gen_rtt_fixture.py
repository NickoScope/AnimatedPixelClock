#!/usr/bin/env python3
"""Synthetic Realtime Trains /gb-nr/location answers, for the host tests.

The api-specification repository carries no example responses - only
specification/main.yml, whose schemas give field names, types and one-value
examples. So these are built from the schema, not captured:

  small()    a hand-made Guildford line-up, one service per rule of the
             transform (cancelled, late, arrived, passing, terminating here,
             starting here, set-down only, bus, joined destinations, a zoned
             time, a time with no zone, ...). Written to samples/.
  big(n)     n services with every field the schema lists for a line-up
             object filled, for sizing the body buffer and the parsed document.

  python3 tools/railboard/gen_rtt_fixture.py     # rewrites samples/rtt_location_small.json
"""
import datetime as dt
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
SMALL = HERE / "samples" / "rtt_location_small.json"
NOW = 1789391107   # 2026-09-14 14:05:07 BST, the samples' clock


def at(hms, zone="+01:00"):
    if len(hms) == 5:
        hms += ":00"
    return f"2026-09-14T{hms}{zone}"


def loc(desc, crs="XXX"):
    return {"namespace": "gb-nr", "description": desc, "shortCodes": [crs], "longCodes": []}


def event(sched, forecast=None, actual=None, cancelled=None):
    e = {"scheduleAdvertised": sched, "realtimeForecast": forecast, "realtimeActual": actual}
    if cancelled is not None:
        e["isCancelled"] = cancelled
    return e


def service(dep=None, arr=None, disp="CALL", call="ADVERTISED_OPEN", platform=None,
            op="SW", mode="TRAIN", pax=True, origin=("Somewhere",), dest=("Elsewhere",)):
    td = {"displayAs": disp, "scheduledCallType": call}
    if dep:
        td["departure"] = dep
    if arr:
        td["arrival"] = arr
    return {
        "temporalData": td,
        "locationMetadata": {"platform": platform or {}},
        "origin": [{"location": loc(o)} for o in origin],
        "destination": [{"location": loc(d)} for d in dest],
        "scheduleMetadata": {"operator": {"code": op, "name": "Operator"}, "modeType": mode,
                             "inPassengerService": pax},
    }


def small():
    s = [
        service(dep=event(at("14:08"), at("14:08")), platform={"actual": "5"}, dest=["London Waterloo"]),
        service(dep=event(at("14:12"), at("14:19")), platform={"planned": 3}, dest=["Portsmouth Harbour"]),
        service(dep=event(at("14:17"), cancelled=True), disp="CANCELLED", platform={"actual": "8"}, op="GW",
                dest=["Reading"]),
        service(dep=event(at("14:10"), at("14:10")), disp="PASS", dest=["Passing Through"]),
        service(dep=event(at("14:11"), at("14:11")), disp=None, dest=["Null Display"]),
        service(dep=event(at("14:30")), mode="REPLACEMENT_BUS", dest=["Farnham"]),
        service(dep=event(at("14:00"), at("14:01"), actual=at("14:01")),
                arr=event(at("13:59"), at("14:04"), actual=at("14:04")),
                platform={"actual": "6"}, op="GW", origin=["Redhill"], dest=["Reading"]),
        service(dep=event(at("14:20")), arr=event(at("14:14"), at("14:14")), disp="TERMINATES",
                platform={"actual": "1"}, origin=["Haslemere"], dest=["Ascot"]),
        service(dep=event(at("14:25"), at("14:25")), arr=event(at("14:21")), disp="STARTS",
                platform={"planned": "4"}, origin=["Woking"], dest=["Aldershot"]),
        service(dep=event(at("14:35")), arr=event(at("14:33")), call="ADVERTISED_SET_DOWN",
                platform={"actual": "7"}, op="GW", origin=["Gatwick Airport"], dest=["Reading"]),
        service(dep=event(at("14:13"), at("14:13")), pax=False, dest=["Empty Stock"]),
        service(dep=event(at("14:40"), at("14:40")), platform={"actual": "10A", "planned": "10"},
                dest=["London Waterloo", "Portsmouth Harbour"]),
        service(dep=event(at("13:45", "Z"), "2026-09-14T13:45:30.000Z"), dest=["Haslemere"]),
        service(dep=event("2026-09-14T14:50:00", "2026-09-14T14:50:00"), dest=["No Zone"]),
        service(dep=event(at("14:02"), at("14:02")), dest=["Gone Already"]),
        service(dep=event(at("14:04:30"), at("14:04:30")), platform={"actual": "2"}, dest=["Woking"]),
        service(dep=event(at("14:55")), platform={"planned": 0}, dest=["Beyond The Eighth"]),
        service(arr=event(at("14:40"), at("14:41")), platform={"planned": 0},
                origin=["Abcdefghijklmnopqrstuvwxyz"]),
        service(dep=event(at("14:15"), at("14:15")), call="OPERATIONAL_ONLY", dest=["Crew Change"]),
    ]
    return {"systemStatus": {"realtimeNetworkRail": "OK", "rttCore": "OK"},
            "query": {"location": loc("Guildford", "GLD"), "timeFrom": at("13:35:07"), "timeTo": at("15:05:07")},
            "services": s}


def big(n, start=NOW - 1800):
    """n services, every field the line-up object's schema lists, realistic lengths."""
    def iso(t):
        return dt.datetime.fromtimestamp(t, dt.timezone(dt.timedelta(hours=1))).isoformat()

    def individual(t):
        return {"scheduleInternal": iso(t - 30), "scheduleAdvertised": iso(t), "realtimeForecast": iso(t + 60),
                "realtimeEstimate": None, "realtimeNoReport": False, "realtimeActual": None,
                "realtimeInternalLateness": None, "realtimeAdvertisedLateness": None, "isCancelled": False}

    def place(desc, crs, tiploc):
        return {"namespace": "gb-nr", "description": desc, "shortCodes": [crs], "longCodes": [tiploc]}

    out = []
    for i in range(n):
        t = start + i * 90
        out.append({
            "temporalData": {"arrival": individual(t - 60), "departure": individual(t), "pass": None,
                             "scheduledCallType": "ADVERTISED_OPEN", "realtimeCallType": "ADVERTISED_OPEN",
                             "displayAs": "CALL", "status": None, "isInterpolated": False},
            "locationMetadata": {"platform": {"planned": "5", "forecast": None, "actual": "5"},
                                 "line": {"planned": "FL", "forecast": None, "actual": None},
                                 "path": {"planned": "SL", "forecast": None, "actual": None},
                                 "numberOfVehicles": 10, "allocationIndex": 0, "isRequestStop": False,
                                 "stockBranding": "South Western Railway",
                                 "allowances": {"engineering": None, "pathing": 30, "performance": None}},
            "reasons": [{"type": "DELAY", "code": "TB", "system": "TRUST", "shortText": "TOC request",
                         "longText": None}] if i % 7 == 0 else [],
            "origin": [{"location": place("Portsmouth Harbour", "PMH", "PMOUTHH"), "temporalData": individual(t - 3600)}],
            "destination": [{"location": place("London Waterloo", "WAT", "WATRLMN"), "temporalData": individual(t + 2400)}],
            "scheduleMetadata": {"uniqueIdentity": f"gb-nr:W{12000 + i}:2026-09-14", "namespace": "gb-nr",
                                 "identity": f"W{12000 + i}", "departureDate": "2026-09-14",
                                 "operator": {"code": "SW", "name": "South Western Railway"}, "modeType": "TRAIN",
                                 "inPassengerService": True, "trainReportingIdentity": f"1P{i % 100:02d}",
                                 "stpIndicator": "WTT", "runsAsRequired": False},
        })
    return {"systemStatus": {"realtimeNetworkRail": "OK", "rttCore": "OK"},
            "query": {"location": place("Guildford", "GLD", "GUILDFD"), "timeFrom": iso(start),
                      "timeTo": iso(start + 5400)},
            "reasons": [], "services": out}


def write_small():
    SMALL.write_text(json.dumps(small(), indent=1) + "\n")


if __name__ == "__main__":
    write_small()
    print(SMALL.relative_to(HERE.parents[1]))
