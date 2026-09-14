#pragma once
// Phase 6b of the bring-up: does Lua disturb the panel? A bench, not a feature:
// built only into env:matrix-waveshare-rgb-luabench.

#if defined(NSLUA_BENCH)

#if !defined(NSLUA_ENABLED)
#error "NSLUA_BENCH needs NSLUA_ENABLED: it benches the Lua runtime"
#endif

void nsluaBenchBegin();       // in setup(), after the Lua self-test
void nsluaBenchLoop();        // first thing in loop(): phases, stalls, report
void nsluaBenchFrameBegin();  // a display frame starts, before anything is drawn
void nsluaBenchFrameEnd();    // the frame has been flipped

#endif
