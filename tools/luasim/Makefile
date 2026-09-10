# Host build of the panel's Lua runtime, for developing effects without hardware.
LUA_DIR := ../../src/lua/vendor/lua
LUA_SRC := $(filter-out $(LUA_DIR)/lua.c $(LUA_DIR)/luac.c, $(wildcard $(LUA_DIR)/*.c))
CFLAGS  := -O2 -I$(LUA_DIR) -Wall -Wno-unused-function

luasim: luasim.c font_picopixel.inc $(LUA_SRC)
	$(CC) $(CFLAGS) -o $@ luasim.c $(LUA_SRC) -lm

font_picopixel.inc: ../../src/fonts/picopixel_fb.h gen_font.py
	python3 gen_font.py

clean:
	rm -f luasim font_picopixel.inc *.raw *.png *.gif
