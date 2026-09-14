#!/usr/bin/env python3
"""Realtime Trains for the rail board, in Python: the token exchange, one
line-up fetch with back-off, and the same transform the firmware
(src/railboard/rtt_transform.cpp) and the earlier Home Assistant template use.

Standard library only, Python 3.8+. Used two ways:

  * imported by the AppDaemon app (tools/railboard/appdaemon/railboard_rtt.py),
    which keeps both tokens in memory - the recommended Home Assistant side;
  * run by Home Assistant's shell_command, the pure-package alternative
    (tools/railboard/ha_package_railboard_shell.yaml):

      python3 /config/railboard/rtt_client.py fetch GLD \\
          [--secrets /config/secrets.yaml] [--key rtt_bearer] \\
          [--state /tmp/railboard_rtt_state.json] [--kind refresh|access|auto]

    It prints one JSON object - the payloads to publish and the outcome - and
    never a token. The access token and the back-off timers are kept between
    runs in --state, a 0600 file in the container's /tmp.

No token is ever logged, printed, or put in a returned value. Checked on the
host by tools/railboard/check_rtt_client.py (a local fake RTT) and
tools/railboard/check_direct.py (lists identical to the C++ transform).

API facts, realtimetrains/api-specification specification/main.yml at 57ace42:
  server https://data.rtt.io (lines 74-76); bearer auth (98-101, 789-790)
  GET /gb-nr/location?code=&timeFrom=&timeWindow= (1018-1169)
  GET /api/get_access_token with the refresh token as bearer ->
      {token, entitlements, validUntil} (1644-1669)
  429 with Retry-After; X-RateLimit-Remaining-<Day...> (37-46)
"""
import argparse
import datetime as _dt
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

API = "https://data.rtt.io"
LOCATION = "/gb-nr/location"
ACCESS_TOKEN = "/api/get_access_token"

LOOKBACK_MIN, WINDOW_MIN, GRACE_S, KEEP = 30, 90, 60, 8
TOKEN_MARGIN_S, TOKEN_ASSUME_S = 300, 600
AUTH_FIRST_S, AUTH_MAX_S = 900, 6 * 3600
RETRY_MIN_S, RETRY_MAX_S, RETRY_NONE_S = 60, 3600, 900

CRS_RE = re.compile(r"[A-Z]{3}")
_TIME_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})[Tt ](\d{2}):(\d{2}):(\d{2})(?:[.,]\d+)?(?:([Zz])|([+-])(\d{2}):?(\d{2}))?")


def _last_sunday_31(y, m):
    d = _dt.date(y, m, 31)
    return (d - _dt.date(1970, 1, 1)).days - (d.weekday() + 1) % 7


def _is_bst(utc):
    # uk_time.h isBst: 01:00 GMT on the last Sundays of March and October.
    y = _dt.datetime.fromtimestamp(utc, _dt.timezone.utc).year
    return _last_sunday_31(y, 3) * 86400 + 3600 <= utc < _last_sunday_31(y, 10) * 86400 + 3600


# ── the transform ───────────────────────────────────────────────────────────
def parse_time(s):
    """RFC 3339 to UTC epoch seconds; None for anything else. A time without a
    zone is London civil time: the live gb-nr answer gives station times with
    neither Z nor an offset (rtt_transform.cpp parseTime, rule for rule)."""
    if not isinstance(s, str):
        return None
    m = _TIME_RE.fullmatch(s)
    if not m:
        return None
    y, mo, d, h, mi, se = (int(m.group(i)) for i in range(1, 7))
    if not (1 <= mo <= 12 and 1 <= d <= 31 and h <= 23 and mi <= 59 and se <= 59):
        return None
    days = (_dt.date(y, mo, 1) - _dt.date(1970, 1, 1)).days + d - 1
    wall = days * 86400 + h * 3600 + mi * 60 + se
    if m.group(7):
        return wall
    if m.group(8):
        oh, om = int(m.group(9)), int(m.group(10))
        if oh > 23 or om > 59:
            return None
        return wall - (oh * 3600 + om * 60) * (-1 if m.group(8) == "-" else 1)
    return wall - 3600 if _is_bst(wall - 3600) else wall


def build_query(crs, now):
    start = _dt.datetime.fromtimestamp(now - LOOKBACK_MIN * 60, _dt.timezone.utc)
    return "code=%s&timeFrom=%s&timeWindow=%d" % (crs, start.strftime("%Y-%m-%dT%H:%M:%SZ"), WINDOW_MIN)


def _s(v):
    return v if isinstance(v, str) and v else None


def _get(d, *keys):
    for k in keys:
        if not isinstance(d, dict):
            return None
        d = d.get(k)
    return d


def _truncate24(s):
    # Jinja's truncate(24, false, '', 0)
    return s if len(s) <= 24 else s[:24].rsplit(" ", 1)[0]


def _platform(v):
    if isinstance(v, bool):
        return None
    if isinstance(v, str) and v:
        return v
    if isinstance(v, int) and v != 0:
        return str(v)
    return None


def normalise(content, now, crs):
    """A /gb-nr/location answer as the panel's two lists, or None when it is not
    one (no services array). The package calls that BAD."""
    services = content.get("services") if isinstance(content, dict) else None
    if not isinstance(services, list):
        return None
    out = {"stn": _s(_get(content, "query", "location", "description")) or crs,
           "rt": _s(_get(content, "systemStatus", "realtimeNetworkRail")) or "",
           "dep": [], "arr": []}
    for sv in services:
        if not isinstance(sv, dict):
            continue
        td = sv.get("temporalData") if isinstance(sv.get("temporalData"), dict) else {}
        disp = td.get("displayAs")
        md = sv.get("scheduleMetadata") if isinstance(sv.get("scheduleMetadata"), dict) else {}
        if disp is None or disp == "PASS" or md.get("inPassengerService", True) is False:
            continue
        if md.get("modeType") in ("BUS", "SCHEDULED_BUS", "REPLACEMENT_BUS"):
            op = "BUS"
        else:
            op = (_s(_get(md, "operator", "code")) or "")[:3]
        pm = _get(sv, "locationMetadata", "platform")
        plat = (_platform(_get(pm, "actual")) or _platform(_get(pm, "planned")) or "")[:3]
        call = td.get("scheduledCallType")
        for side in ("dep", "arr"):
            ev = td.get("departure" if side == "dep" else "arrival")
            wrong = "ADVERTISED_SET_DOWN" if side == "dep" else "ADVERTISED_PICK_UP"
            if not isinstance(ev, dict) or not _s(ev.get("scheduleAdvertised")) or call in ("OPERATIONAL_ONLY", wrong):
                continue
            t = parse_time(ev.get("scheduleAdvertised")) or 0
            xs = _s(ev.get("realtimeActual")) or _s(ev.get("realtimeForecast")) or _s(ev.get("realtimeEstimate"))
            x = (parse_time(xs) or 0) if xs else 0
            actual = ev.get("realtimeActual") is not None
            canc = ev.get("isCancelled") is True or disp in ("CANCELLED", "DIVERTED") or \
                disp == ("TERMINATES" if side == "dep" else "STARTS")
            when = x or t
            if side == "dep":
                show = t > 0 and not (actual and not canc) and when >= now - GRACE_S
            else:
                show = t > 0 and when >= now - (2 * GRACE_S if actual else GRACE_S)
            if not show:
                continue
            places = sv.get("destination" if side == "dep" else "origin") or []
            names = [_s(_get(e, "location", "description")) for e in places if isinstance(e, dict)]
            late = (x // 60 - t // 60) if x else 0
            if canc:
                st = "canc"
            elif side == "arr" and actual:
                st = "arr"
            elif not x:
                st = "nr"
            else:
                st = "late" if late >= 1 else "ok"
            out[side].append({"t": t, "x": x, "p": plat, "n": _truncate24(" & ".join(n for n in names if n)),
                              "o": op, "st": st, "d": max(late, 0) if st in ("late", "arr") else 0})
    for side in ("dep", "arr"):
        out[side] = sorted(out[side], key=lambda r: r["t"])[:KEEP]
    return out


def board_payloads(crs, lists, ts):
    """The retained payloads the panel reads, schema v1 (src/railboard/README.md)."""
    return {leaf: {"v": 1, "crs": crs, "stn": lists["stn"], "dir": d, "ts": int(ts), "rt": lists["rt"],
                   "s": lists[d]}
            for leaf, d in (("departures", "dep"), ("arrivals", "arr"))}


def status_payload(result, at):
    return {"v": 1, "at": int(at), "err": result["err"], "code": int(result["code"]),
            "retry": int(result["retry"]), "rl": result["rl"]}


# ── the client ──────────────────────────────────────────────────────────────
def _headers(h):
    return {k.lower(): v for k, v in (h.items() if h is not None else [])}


def _int(v):
    try:
        return int(str(v).strip())
    except (TypeError, ValueError):
        return 0


def _failure(code):
    if code == 0:
        return "NET"
    if code in (401, 403):
        return "AUTH"
    if code == 429:
        return "RATE"
    return "HTTP"


class RttClient:
    """Tokens stay inside this object: no method returns, logs or prints one."""

    def __init__(self, token, kind="auto", base=None, timeout=15, clock=time.time):
        if kind not in ("auto", "refresh", "access"):
            raise ValueError("kind must be auto, refresh or access")
        token = (token or "").strip()
        if token.startswith("Bearer "):
            token = token[len("Bearer "):].strip()
        self._stored = token
        self._kind_cfg = kind
        self._base = base
        self._timeout = timeout
        self._clock = clock
        self._access = None
        self._access_until = 0
        self.kind = "unknown"
        self._auth_step = 0
        self.auth_until = 0
        self.retry_until = 0
        self.requests = 0

    def __repr__(self):
        return "RttClient(<tokens hidden>)"

    # State for a process that runs once per poll (the shell_command path).
    def state(self):
        return {"access": self._access, "access_until": self._access_until, "kind": self.kind,
                "auth_step": self._auth_step, "auth_until": self.auth_until, "retry_until": self.retry_until}

    def restore(self, st):
        if not isinstance(st, dict):
            return
        self._access = st.get("access") if isinstance(st.get("access"), str) else None
        self._access_until = _int(st.get("access_until"))
        self.kind = st.get("kind") if st.get("kind") in ("unknown", "access", "refresh-exchanged", "refused") else "unknown"
        self._auth_step = _int(st.get("auth_step"))
        self.auth_until = _int(st.get("auth_until"))
        self.retry_until = _int(st.get("retry_until"))

    def _request(self, path, bearer):
        req = urllib.request.Request((self._base or API) + path, headers={
            "Authorization": "Bearer " + bearer, "Accept": "application/json",
            "User-Agent": "nickoscope-railboard/1"})
        self.requests += 1
        try:
            with urllib.request.urlopen(req, timeout=self._timeout) as r:
                return r.status, _headers(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, _headers(e.headers), b""
        except (urllib.error.URLError, OSError, ValueError):   # DNS, refused, TLS, timeout
            return 0, {}, b""

    def _exchange(self):
        code, hdrs, body = self._request(ACCESS_TOKEN, self._stored)
        if code == 200:
            try:
                data = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                data = None
            tok = data.get("token") if isinstance(data, dict) else None
            if isinstance(tok, str) and tok:
                now = self._clock()
                until = parse_time(data.get("validUntil"))
                self._access = tok
                self._access_until = until if until and until > now else int(now) + TOKEN_ASSUME_S
                self.kind = "refresh-exchanged"
                return code, hdrs, ""
            return code, hdrs, "BAD"
        if code in (401, 403):
            self._access, self._access_until, self.kind = None, 0, "refused"
            return code, hdrs, "AUTH"
        return code, hdrs, _failure(code)

    def fetch(self, crs, now=None):
        """One poll. A dict: code, err ('' | AUTH | RATE | HTTP | BAD | NET), retry,
        rl (remaining today), limit, kind, backoff (seconds still to wait, or 0),
        requests (made by this call), content (the answer on success)."""
        now = int(self._clock() if now is None else now)
        before = self.requests
        res = {"code": 0, "err": "", "retry": 0, "rl": "", "limit": "", "kind": self.kind,
               "backoff": 0, "requests": 0, "content": None}
        if not self._stored:
            res.update(err="AUTH", code=401)
            return res
        if self.auth_until > now:          # a refused token: no request until the back-off ends
            res.update(err="AUTH", code=401, backoff=self.auth_until - now)
            return res
        if self.retry_until > now:         # RTT said when to come back
            res.update(err="RATE", code=429, backoff=self.retry_until - now)
            return res

        use_access = self._access is not None and now < self._access_until - TOKEN_MARGIN_S
        if self._kind_cfg == "refresh" and not use_access:
            code, hdrs, err = self._exchange()
            if err:
                return self._finish(res, code, hdrs, err, now, before)
            use_access = True
        path = LOCATION + "?" + build_query(crs, now)
        code, hdrs, body = self._request(path, self._access if use_access else self._stored)
        if code == 401 and self._kind_cfg != "access":
            if use_access:
                self._access = None
            code2, hdrs2, err = self._exchange()
            if err:
                return self._finish(res, code2, hdrs2, err, now, before)
            code, hdrs, body = self._request(path, self._access)
            use_access = True

        if code == 204:
            err, res["content"] = "", {"services": []}
        elif code == 200:
            try:
                content = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                content = None
            if isinstance(content, dict) and isinstance(content.get("services"), list):
                err, res["content"] = "", content
            else:
                err = "BAD"
        else:
            err = _failure(code)
        if not err and not use_access and self.kind == "unknown":
            self.kind = "access"
        return self._finish(res, code, hdrs, err, now, before)

    def _finish(self, res, code, hdrs, err, now, before):
        retry = _int(hdrs.get("retry-after"))
        res.update(code=code, err=err, retry=retry, rl=str(hdrs.get("x-ratelimit-remaining-day", "")),
                   limit=str(hdrs.get("x-ratelimit-limit-day", "")))
        if err == "AUTH":
            self._auth_step = min(self._auth_step * 2, AUTH_MAX_S) if self._auth_step else AUTH_FIRST_S
            self.auth_until = now + self._auth_step
            res["backoff"] = self._auth_step
        elif err == "RATE":
            wait = min(max(retry, RETRY_MIN_S), RETRY_MAX_S) if retry else RETRY_NONE_S
            self.retry_until = now + wait
            res["backoff"] = wait
        elif not err:
            self._auth_step = 0
        if err:
            res["content"] = None
        res["kind"] = self.kind
        res["requests"] = self.requests - before
        return res


# ── the shell_command path ──────────────────────────────────────────────────
def read_secret(path, key):
    """One `key: value` line of a secrets.yaml, quoted or plain. Not a YAML parser:
    anything fancier than one line is refused."""
    found = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith(key + ":"):
                found.append(line.split(":", 1)[1].strip())
    if len(found) != 1:
        raise LookupError("expected one %s: line, found %d" % (key, len(found)))
    v = found[0]
    if v[:1] in ("'", '"'):
        end = v.find(v[0], 1)
        if end < 0:
            raise LookupError("unterminated quote on the %s: line" % key)
        return v[1:end]
    return re.split(r"\s+#", v, maxsplit=1)[0].strip()


def _load_state(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _save_state(path, st):
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(st, f)
    os.replace(tmp, path)


def _cli(argv):
    ap = argparse.ArgumentParser(prog="rtt_client.py")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fetch")
    p.add_argument("crs")
    p.add_argument("--secrets", default="/config/secrets.yaml")
    p.add_argument("--key", default="rtt_bearer")
    p.add_argument("--state", default="/tmp/railboard_rtt_state.json")
    p.add_argument("--kind", choices=("auto", "refresh", "access"), default="refresh")
    a = ap.parse_args(argv)
    now = int(time.time())
    out = {"ts": now, "crs": a.crs}
    try:
        if not CRS_RE.fullmatch(a.crs):
            raise ValueError("crs")
        client = RttClient(read_secret(a.secrets, a.key), kind=a.kind)
        client.restore(_load_state(a.state))
        res = client.fetch(a.crs, now)
        _save_state(a.state, client.state())
        out.update({k: res[k] for k in ("err", "code", "retry", "rl", "limit", "kind", "backoff", "requests")})
        if not res["err"]:
            lists = normalise(res["content"], now, a.crs)
            if lists is None:
                out["err"] = "BAD"
            else:
                out["departures"], out["arrivals"] = (board_payloads(a.crs, lists, now)[k] for k in ("departures", "arrivals"))
        out["status"] = status_payload(out, now)
    except Exception as e:   # the type only: a message could quote a secrets line
        out.update(err="BAD", code=0, retry=0, rl="", error=type(e).__name__)
        out["status"] = status_payload(out, now)
    sys.stdout.write(json.dumps(out, separators=(",", ":")) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
