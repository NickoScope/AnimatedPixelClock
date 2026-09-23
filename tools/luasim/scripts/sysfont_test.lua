-- sysfont_test.lua - a test card for the system font through px.text's font
-- argument: the classic 5x7 ("5x7") and Picopixel with its own lowercase
-- ("pico"), Latin and Cyrillic, capitals and lowercase (src/fonts/sys_text.h).
-- Not shipped (gen_effects.py SKIP); fx_parity renders it through both the
-- panel's code and luasim, so the two must agree on every pixel here.

local up5, low5 = "ПРИВЕТ", "привет"
assert(px.width(up5, "5x7") == 36 and px.width(low5, "5x7") == 36, "5x7 is 6 a letter")
assert(px.width("A", "pico") == px.width("А", "pico"), "Latin A and Cyrillic А are one glyph")
assert(px.width("ПРИВЕТ") == px.width("привет"), "the default still folds case")

local rows = {
  { "5x7",  0, "АБВГДЕЁЖЗИЙКЛМНОПРСТ", 255, 255, 255 },
  { "5x7",  8, "УФХЦЧШЩЪЫЬЭЮЯ Hi 21°", 255, 255, 255 },
  { "5x7", 16, "абвгдеёжзийклмнопрст", 120, 200, 255 },
  { "5x7", 24, "уфхцчшщъыьэюя quick",  120, 200, 255 },
  { "pico", 33, "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХ",   255, 190, 40 },
  { "pico", 40, "ЦЧШЩЪЫЬЭЮЯ Abc xyz",        255, 190, 40 },
  { "pico", 47, "абвгдеёжзийклмнопрстуфх",   140, 255, 140 },
  { "pico", 54, "цчшщъыьэюя съешь же ещё",   140, 255, 140 },
}

function draw()
  px.clear(0, 0, 0)
  for _, r in ipairs(rows) do
    px.text(1, r[2], r[3], r[4], r[5], r[6], r[1])
  end
  px.rect(1, 62, px.width(low5, "5x7"), 1, 255, 90, 90, true)
end
