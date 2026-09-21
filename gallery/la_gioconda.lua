-- @upload-only
-- LA GIOCONDA - the painting drawn in ASCII characters.
--
-- Four libraries were read before this, and none of them converts an image.
-- asciicker is a 3D game whose cells come from rasterising geometry - it never
-- loads a picture. libtcod's generate_quadrant_graphic (image_c.c:711-805) is
-- an excellent converter but it converts to QUADRANT BLOCKS, not to type.
-- SadConsole averages a cell to one colour and picks a block by brightness.
-- BearLibTerminal has no converter at all.
--
-- The one that actually draws pictures with characters is chafa
-- (hpjansson/chafa), and it is far better at it than anything fitted by hand
-- here. Per cell it searches the whole symbol set for the glyph whose bitmap
-- best matches which of the cell's pixels belong to the foreground - a Hamming
-- distance over the glyph coverage - and evaluates the colours jointly with
-- that choice rather than picking density first and colour after.
--
-- The part that made it work on this panel: chafa's --glyph-file takes any font
-- FreeType can read, so PicopixelFB was written out as a BDF and handed to it.
-- Matching against some other font's idea of an S would have put a character on
-- the panel that does not look like what was matched. And because px.text
-- upper-cases before it looks a glyph up (lua_px.cpp:202), the BDF gives every
-- lowercase code the uppercase shape - which is true of this panel and lets
-- chafa use the whole range honestly.
--
--   tools/luasim/mkbdf.py      PicopixelFB -> picopixel.bdf
--   chafa --glyph-file picopixel.bdf --symbols ascii --size 21x12 \
--         -f symbols --font-ratio 4/5 --fg-only -c truecolor in.png
--   tools/luasim/chafa_to_lua.py   its ANSI output -> the table below
--
-- 21 x 12 characters on a 4 x 5 grid: 84 x 60 of the panel's 128 x 64. Every
-- cell is one px.text call in the colour chafa chose. The painting is drawn
-- once - this firmware does not clear the canvas between frames - and only the
-- clock is repainted after that.

PERIOD = 60.0
FPS = 4

local ART_W, ART_H = 21, 12
local ART_G = {
  "UUUUUUUUUUUUUUUUaUaUU",
  "UUUUUUUbe^ ^9aUaUUUaU",
  "aUUUUUkUU$l   4aUUUUa",
  "UaUUUFUaaaal   9hahah",
  "haUae hhhaaaI   aUahh",
  "dbaaF UUeUeUI    Uehh",
  "[  '' habhae'   ^444h",
  "`     jUbUeb        _",
  "  _U.   aaF       a'-",
  "Uaaa6    l__l     =l_",
  ", ^`   _UUUUUl    _{z",
  "P     UUaaUaeb     ja",
}
local ART_F = {
  "8297358094337C8E2E7B8C29798E2D7E932C829129818E2B829533829637849B3E7F93377B8F327B93357D9C417F98387D90348093348091347E92387D9C46",
  "92A03C95A23D99A73F98A53896A23198A4359AA23599922A896D2194993892983A919A3A7A742787943590A33F8EA13B8D98318E9B398D9B3F8E9F4193A544",
  "A7AE44AAB24DACB44BA7AF43AEB541AAB4519B8127AA680CC5810EB169058F46028F46028F46028F46028B88289CAE4A98AA4A99A8439AAD51A4B34DAEB544",
  "BFC352BFC04DBAC056BABC4BBBC260A7A74EC8830DF9DC3EFAE439FBD425E59905A05200A05200A05200A052008D974199AC4EA9B54DADBC51B1BB53B9BF53",
  "ADB446BCBF5AC9CD68D0CF5CBCB244BCB244E59F1DF4C32BFAD12DEAB01BE69F13CB7E07984D00984D00984D00984D00616E207488337FA14396A643C4C44F",
  "606824737624A7AB4B999D4992903D92903DD58D17D4870DEBAA15D4890BCF880EC57B0BB86703B86703B86703B86703B8670343642E486E37547333B7BB47",
  "525D1F525D1F525D1F766311624F18624F18E8A01BF6BD28EDB126EAAA1EF9C226D58508A24F00A24F00A24F00A24F004D5D2B52642C496F38507639738F41",
  "556025556025556025556025556025556025A15608F0A714CB7407C5730AE2920FA35403A35403A35403A35403A35403A35403A35403A35403A35403375938",
  "FFFFFF0000004D592C585C27707837707837707837CD8012DA9815BA6A0AA45504A45504A45504A45504A45504A45504A45504A455044B5C26485622335338",
  "4E5C3155683E61703F6874395F602A5F602A5F602A5F602A5F602AAC690DA66004A35B03894A05894A05894A05894A05894A05894A054D6023475F2A697A37",
  "744B08575A2D645D29636730636730636730636730D68C0AB47915EBAE1AE3A215E3A718E09C10BB6903BB6903BB6903BB6903BB69035E50165D5F1E65631C",
  "8B56048B56048B56048B56048B56048B5604B05E09F7CE2EFBE23DFCE335FCDF31FCDF37F8C51EB96103B96103B96103B96103B96103B96103846410A28518",
}

local CW, CH = 4, 5              -- three pixels of glyph and one of air
local ART_X, ART_Y = 0, 2
local CLK_X, CLK_W = 86, 42

local GOLD  = {238, 202, 124}
local OCHRE = {170, 126,  60}
local DIM   = { 44,  34,  22}

-- The digits are characters too: each lit square of a 3x5 digit is one glyph,
-- and 'U' is the densest the panel's font offers inside a cell (11 of 15
-- pixels). Nothing in ASCII is solid, so a digit drawn this way is the closest
-- a character grid gets to a stroke.
local INK = "U"

local DIGITS = {
  [0] = {7,5,5,5,7}, [1] = {2,6,2,2,7}, [2] = {7,1,7,4,7}, [3] = {7,1,3,1,7},
  [4] = {5,5,7,1,1}, [5] = {7,4,7,1,7}, [6] = {7,4,7,5,7}, [7] = {7,1,1,1,1},
  [8] = {7,5,7,5,7}, [9] = {7,5,7,1,7},
}

local CELL = {}
for r = 1, ART_H do
  local g, f = ART_G[r], ART_F[r]
  local row = {}
  for c = 1, ART_W do
    local h = (c - 1) * 6
    row[c] = { g:sub(c, c),
      tonumber(f:sub(h+1,h+2),16), tonumber(f:sub(h+3,h+4),16), tonumber(f:sub(h+5,h+6),16) }
  end
  CELL[r] = row
end
ART_G, ART_F = nil, nil

local function portrait()
  for r = 1, ART_H do
    local row, y = CELL[r], ART_Y + (r - 1) * CH
    for c = 1, ART_W do
      local u = row[c]
      if u[1] ~= " " then
        -- px.text puts a glyph's ink two rows below the y it is given
        px.text(ART_X + (c - 1) * CW, y - 2, u[1], u[2], u[3], u[4])
      end
    end
  end
end

local function digit(n, x, y, r, g, b)
  local rows = DIGITS[n]
  for ry = 1, 5 do
    local bits = rows[ry]
    for rx = 1, 3 do
      if bits % (2 ^ (4 - rx)) >= (2 ^ (3 - rx)) then
        px.text(x + (rx - 1) * CW, y + (ry - 1) * CH - 2, INK, r, g, b)
      end
    end
  end
end

local DW = 3 * CW
local first = true

function draw()
  if first then
    px.clear(0, 0, 0)
    portrait()
    first = false
  end

  local t = px.now()
  local phase = px.t()
  px.rect(CLK_X - 2, 0, 128 - CLK_X + 2, 64, 0, 0, 0, true)

  local PAIR = DW * 2 + CW
  local x = CLK_X + (((CLK_W - PAIR) // 2) // CW) * CW
  digit(math.floor(t.hour / 10) % 10, x,           4,  GOLD[1], GOLD[2], GOLD[3])
  digit(t.hour % 10,                  x + DW + CW, 4,  GOLD[1], GOLD[2], GOLD[3])
  digit(math.floor(t.min / 10) % 10,  x,           34, OCHRE[1], OCHRE[2], OCHRE[3])
  digit(t.min % 10,                   x + DW + CW, 34, OCHRE[1], OCHRE[2], OCHRE[3])

  -- The minute, in characters along the foot of the clock half.
  local cells = CLK_W // CW
  local lit = phase * cells
  for i = 1, cells do
    local bx = CLK_X + (i - 1) * CW
    if i <= lit then
      if i > lit - 1 then px.text(bx, 57, INK, GOLD[1], GOLD[2], GOLD[3])
      else px.text(bx, 57, "_", OCHRE[1], OCHRE[2], OCHRE[3]) end
    else
      px.text(bx, 57, ".", DIM[1], DIM[2], DIM[3])
    end
  end
end
