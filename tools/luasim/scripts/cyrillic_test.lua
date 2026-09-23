-- cyrillic_test.lua - a test card for text: Picopixel's Latin with the
-- Cyrillic added in src/fonts (PicopixelCyr, tools/fonts/mkcyr.py).
-- Not shipped (gen_effects.py SKIP); fx_parity renders it through both the
-- panel's code and luasim, so the two must agree on every pixel here.
--
-- Row by row: the capitals; the same in lowercase, which the font folds to
-- capitals; a mixed Latin/Cyrillic line with a degree sign the font lacks
-- (drawn as the missing box); a pangram; broken UTF-8 bytes, each maximal
-- subpart one box; and two bars whose lengths are px.width of the same word
-- in upper and lower case, which must be equal.

local W, H = px.size()

local up, low = "ПРИВЕТ", "привет"
assert(px.width(up) == px.width(low), "case folding changed a width")
assert(px.width("A") == px.width("А"), "Latin A and Cyrillic А must be one glyph")

local lines = {
  { "АБВГДЕЁЖЗИЙКЛМН",       255, 255, 255 },
  { "ОПРСТУФХЦЧШЩЪЫЬЭЮЯ",    255, 255, 255 },
  { "абвгдеёжзийклмн",       120, 200, 255 },
  { "опрстуфхцчшщъыьэюя",    120, 200, 255 },
  { "WiFi: Кухня 21°C",      255, 190,  40 },
  { "Съешь же ещё этих",     140, 255, 140 },
  { "мягких булок 2026",     140, 255, 140 },
  { "A\255B\208Z\226\130",   255,  90,  90 },
}

function draw()
  px.clear(0, 0, 0)
  for i, l in ipairs(lines) do
    px.text(1, (i - 1) * 7, l[1], l[2], l[3], l[4])
  end
  local y = #lines * 7 + 1
  px.rect(1, y, px.width(up), 2, 255, 255, 255, true)
  px.rect(1, y + 3, px.width(low), 2, 120, 200, 255, true)
end
