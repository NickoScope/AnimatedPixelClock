"""The one HTTP door: urllib, the User-Agent from the settings, JSON or text back, 429 -> RateLimited.

Named httpio, not http (audit M7): AppDaemon's legacy import method puts every
directory of the apps tree on sys.path (app_management._process_import_paths, read
through deepwiki on 2026-09-15), so a module named http.py in this package would
shadow the standard library's http package for the whole AppDaemon process.

Kept apart so tests hand the providers a FakeHttp with canned answers and no socket
is ever opened in a test. Errors carry the URL's path and the code, never a query
string (a crumb would be one). A 429 is logged with the User-Agent and the first
bytes of the body: Yahoo's answer depends on the User-Agent (audit B2).
"""
import http.client
import http.cookiejar
import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Callable, Dict, Optional

from .base import NotFound, ProviderError, RateLimited

# The User-Agent Yahoo answered with 200: measured by the coordinator on 2026-09-15 12:20, when Python's
# default and a full Safari 17 string both got 429. Yahoo can change this at any time, so it is the
# APP_SCHEMA setting `user_agent`; this is only its default.
DEFAULT_UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/537.36"
BODY_LOG_BYTES = 80


def _snippet(e) -> str:
    """The first bytes of an error's body, printable ASCII only."""
    try:
        raw = e.read(BODY_LOG_BYTES) or b""
    except Exception:
        raw = b""
    return "".join(ch if 32 <= ord(ch) < 127 else "." for ch in raw[:BODY_LOG_BYTES].decode("latin-1"))


class Http:
    def __init__(self, timeout: float = 20.0, user_agent: Optional[str] = None, log: Optional[Callable[..., None]] = None):
        self.timeout, self.ua = timeout, user_agent or DEFAULT_UA
        self.log = log or (lambda *a, **k: None)
        self.calls = 0
        # one cookie jar for the session: Yahoo's crumb is tied to its cookie
        self.jar = http.cookiejar.CookieJar()
        self._opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def get(self, url: str, params: Optional[Dict[str, object]] = None, headers: Optional[Dict[str, str]] = None) -> bytes:
        full = url + ("?" + urllib.parse.urlencode(params) if params else "")
        h = {"User-Agent": self.ua, "Accept": "application/json,text/csv,*/*"}
        h.update(headers or {})
        req = urllib.request.Request(full, headers=h)
        self.calls += 1
        where = urllib.parse.urlsplit(url).path
        try:
            with self._opener.open(req, timeout=self.timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 429:
                ra = e.headers.get("Retry-After") if e.headers else None
                try:
                    ra_s = float(ra) if ra else None
                except ValueError:
                    ra_s = None
                self.log("market: HTTP 429 on %s with User-Agent %r; body %r" % (where, self.ua, _snippet(e)), level="WARNING")
                raise RateLimited("HTTP 429 on %s" % where, ra_s) from None
            if e.code == 404:
                raise NotFound("HTTP 404 on %s" % where) from None
            raise ProviderError("HTTP %d on %s" % (e.code, where)) from None
        except urllib.error.URLError as e:
            raise ProviderError("%s on %s" % (type(e.reason).__name__ if e.reason else "URLError", where)) from None
        except http.client.HTTPException as e:          # IncompleteRead, BadStatusLine: not OSError (audit MINOR 8)
            raise ProviderError("%s on %s: %s" % (type(e).__name__, where, str(e)[:80])) from None
        except (TimeoutError, OSError, ValueError) as e:
            raise ProviderError("%s on %s" % (type(e).__name__, where)) from None

    def get_json(self, url: str, params: Optional[Dict[str, object]] = None, headers: Optional[Dict[str, str]] = None) -> dict:
        raw = self.get(url, params, headers)
        try:
            return json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            raise ProviderError("not JSON from %s" % urllib.parse.urlsplit(url).path) from None

    def get_text(self, url: str, params: Optional[Dict[str, object]] = None, headers: Optional[Dict[str, str]] = None) -> str:
        return self.get(url, params, headers).decode("utf-8", errors="replace")
