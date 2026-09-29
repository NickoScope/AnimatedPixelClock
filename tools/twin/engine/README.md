# The twin's engine

The emulator is our fork of esp32sim (Joakim Eriksson, MIT):
**https://github.com/NickoScope/esp32sim**, branch `nickoscope/twin` (the fork's default branch).
`main` there follows upstream; `NICKOSCOPE.md` there lists what the branch adds.

The twin was last checked with commit `ed87818` of that branch. To get it:

```
git clone -b nickoscope/twin https://github.com/NickoScope/esp32sim ~/twin/esp32sim
cd ~/twin/esp32sim && cargo build --release
```

Until 2026-09-29 the engine lived here as one patch against upstream 4ab7e90; the owner chose a
fork instead, so the history is readable and changes can go upstream as pull requests.
