#pragma once
// An example of src/market/market_local_defaults.h, the uncommitted header
// that replaces the neutral defaults of market_settings.cpp in one's own
// build. Copy it there (the name is in .gitignore) and put your own rows in.
//
// Each row is a setting's dotted key and its default in the text form
// src/market/README.md lists ("SYM=NAME,..." for a symbol list,
// "SYM:W[:DATE[:CLASS[:PROXY]]],..." for the allocation). A key not listed
// keeps the neutral default; a row that fails its check falls back to it and
// the boot log says which (market_ha.cpp, mks::checkLocalDefaults).
//
// This file is also what the host test and the flag matrix build the
// mechanism with (-DMARKET_LOCAL_DEFAULTS_FILE); the funds are an example
// three-fund allocation, not anyone's.

static const mks::LocalDefault kLocalDefaults[] = {
  {"tickers", "VTI=VTI,VXUS=VXUS,BND=BND"},
  {"display.ticker", "VXUS"},
  {"portfolio.positions", "VTI:50::equity,VXUS:30::equity,BND:20::bond"},
};
