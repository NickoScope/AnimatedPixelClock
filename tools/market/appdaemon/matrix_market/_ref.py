"""Finds tools/market/market_ref.py, the maths, wherever this package is installed.

Three places, in order: a copy inside this package (the install step may drop
market_ref.py next to app.py), the import path (AppDaemon puts every app
directory on sys.path, so market_ref.py in the apps directory works too), and
the repository layout (tools/market/appdaemon/matrix_market -> tools/market).
The module object is `market_ref`; every other module imports it from here so
the choice is made once.
"""
import importlib
import pathlib
import sys

try:
    from . import market_ref  # type: ignore  # a copy inside the package
except ImportError:
    try:
        market_ref = importlib.import_module("market_ref")
    except ImportError:
        _repo = pathlib.Path(__file__).resolve().parents[2]      # tools/market
        if str(_repo) not in sys.path:
            sys.path.insert(0, str(_repo))
        market_ref = importlib.import_module("market_ref")

MARKET_REF_PATH = pathlib.Path(market_ref.__file__).resolve()
