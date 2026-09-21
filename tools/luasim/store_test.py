#!/usr/bin/env python3
"""The uploaded-script checks, compiled for the host and driven with the cases
that once got past them.

Every refusal case below is a real defect this project shipped or nearly
shipped. The first version of luaStoreValidate counted brackets only, stopped
at an unterminated `--[[` and returned true, and knew nothing of levelled long
brackets - so `[=[ )))) ]=]` lowered its counter and a script could measure
shallower than it was. And it measured the wrong thing entirely: nested
`local function` is the most expensive nesting Lua's parser performs, at 272
bytes of C stack a level, and it contains no bracket at all.

It compiles the real src/lua/lua_store.cpp against small stubs in host/store/,
so what is tested is the code that ships rather than a copy of it.

    python3 tools/luasim/store_test.py
"""
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent


def main():
    out = HERE / ".store_test"
    cmd = ["c++", "-std=gnu++11", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-I", str(HERE / "host" / "store"), "-I", str(HERE.parent.parent / "src" / "lua"),
           "-o", str(out), str(HERE / "store_test.cpp")]
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit("store_test: did not compile")
    sys.exit(subprocess.run([str(out)]).returncode)


if __name__ == "__main__":
    main()
