"""matrix_market: the market dashboard's Home Assistant side, as an AppDaemon package.

One module per concern, wired by a small registry, so a feature, a provider, a
metric or a setting can be added or removed without touching the others:

  app.py            the AppDaemon class; wires only the features apps.yaml lists
  config.py         the declarative settings schema; validation and defaults come from it
  registry.py       the registries: features, providers, modes, metrics, windows, exchanges
  providers/        Provider interface (base), Yahoo (yahoo), ECB rates (ecb), fixtures
  store.py          versioned per-symbol files, append-only bars, a manifest
  timeline.py       the trading-day timeline the ledger runs on
  windows.py        presets (YTD..MAX) and the per-window metrics table
  encode.py         the 128-point uint16 base64 line and its decoder
  ledger.py         the thin adapter over tools/market/market_ref.py (the maths, once)
  publisher.py      MQTT topics, retained payloads, the size guard, generations
  history.py        feature: the daily fetch, the store, index/ticker payloads, status
  portfolio.py      feature: both ledger modes, portfolio and holdings payloads
  live.py           feature: the session loop (spark quotes, the selected ticker's intraday)
  tape.py           feature: exchange states for the tape
  ha_entities.py    feature: MQTT discovery of the Home Assistant sensors
  cli.py            fetch / compute / publish --dry-run / report, for the laptop

The design is docs/18-stock-dashboard.md in the knowledge base. Nothing here
reimplements the ledger: ledger.py calls market_ref.

AppDaemon loads this directory as a package because of this file: apps.yaml
names `module: matrix_market.app` (APPGUIDE, "App Packages"), and the modules
import each other relatively (`from .store import Store`).
"""
__version__ = "0.2.0"
SCHEMA_V = 1          # the `v` of every payload the app publishes (docs/18, "MQTT contract")
CONFIG_V = 2          # the `v` of the panel's `config` the schema understands (docs/18, "Settings after the research": v2)
