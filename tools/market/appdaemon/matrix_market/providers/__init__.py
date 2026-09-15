"""Data providers: the seam the design asks for so the source can change in one file.

base.py     the Provider interface, the value types, the errors, the pacer and the back-off
httpio.py   the one HTTP door (urllib, the User-Agent setting, 429 -> RateLimited), fakeable; not http.py (it would shadow the stdlib)
yahoo.py    Yahoo Finance: chart v8 for history and intraday, spark for quotes, crumb +
            quoteSummary (or yfinance when installed) for the expense ratio
ecb.py      the ECB Data Portal: EUR/USD EXR.M.USD.EUR.SP00.E through market_ref's table, and the HICP; cached in the store
fixtures.py the saved answers in tools/market/samples/, for tests and the CLI's --offline

A provider registers itself in registry.PROVIDERS under the name apps.yaml uses
(`provider: yahoo`).
"""
from . import fixtures, yahoo  # noqa: F401  (registrations)
