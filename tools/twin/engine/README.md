# The twin's engine

The emulator is our fork of esp32sim (Joakim Eriksson, MIT):
**https://github.com/NickoScope/TWIN-NickoScopeMatrix-64x128**, branch `nickoscope/twin` (the fork's default branch).
`main` there follows upstream; `NICKOSCOPE.md` there lists what the branch adds.

The twin was last checked with commit `656cd2a` of that branch (2026-09-30: 16 MB PSRAM as the panel sees it, the MWDT watchdogs, real-time pacing that catches a lag up instead of dropping it; 612 engine tests pass, the wasm-jit ones need Node; 24 min of heavy Lua effects on 2.7.6 with no watchdog reset). To get it:

```
git clone -b nickoscope/twin https://github.com/NickoScope/TWIN-NickoScopeMatrix-64x128 ~/twin/esp32sim
cd ~/twin/esp32sim && cargo build --release
```

Until 2026-09-29 the engine lived here as one patch against upstream 4ab7e90; the owner chose a
fork instead, so the history is readable and changes can go upstream as pull requests.
