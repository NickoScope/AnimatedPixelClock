// ============================================================
// nslua_sandbox.h - the one whitelist every nslua lua_State opens
// ============================================================
// Moved out of nslua.cpp unchanged, so that the stateless self-test
// (nslua_run) and the persistent effect runtime (lua_fx.cpp) cannot drift
// apart: base, table, string, math; dofile, loadfile, load and
// collectgarbage removed; print and log to the console; require resolves
// only modules already in package.loaded.
//
// No instruction hook and no binding groups here - each caller adds its own.
// Host-portable (no Arduino.h), so the effect runtime can be checked on a Mac.
// Call under lua_pcall: opening the libraries allocates and may raise OOM.
// ============================================================
#ifndef NSLUA_SANDBOX_H
#define NSLUA_SANDBOX_H

struct lua_State;

#ifdef __cplusplus
extern "C" {
#endif

void nslua_sandbox_open(struct lua_State *L);

// Message handler for lua_pcall: the error text plus a traceback.
int nslua_message_handler(struct lua_State *L);

#ifdef __cplusplus
}
#endif

#endif  // NSLUA_SANDBOX_H
