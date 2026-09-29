# The twin's engine

The emulator is our fork of esp32sim (Joakim Eriksson, MIT):
**https://github.com/NickoScope/TWIN-NickoScopeMatrix-64x128**, branch `nickoscope/twin` (the fork's default branch).
`main` there follows upstream; `NICKOSCOPE.md` there lists what the branch adds.

The twin was last checked with commit `e01c301` of that branch (2026-09-29: `check_flasher.py` 25/25, the engine's tests pass except the wasm-jit ones that need Node). To get it:

```
git clone -b nickoscope/twin https://github.com/NickoScope/TWIN-NickoScopeMatrix-64x128 ~/twin/esp32sim
cd ~/twin/esp32sim && cargo build --release
```

Until 2026-09-29 the engine lived here as one patch against upstream 4ab7e90; the owner chose a
fork instead, so the history is readable and changes can go upstream as pull requests.
