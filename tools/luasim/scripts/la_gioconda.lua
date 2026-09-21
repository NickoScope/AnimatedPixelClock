-- LA GIOCONDA - a clock that draws the Mona Lisa the way asciicker draws a
-- world: not a character on a background, but two colours per cell and a glyph
-- that says where the line between them runs.
--
-- asciicker (msokalski/asciicker, render.cpp) fills an AnsiCell with fg, bk and
-- a CP437 glyph, and AverageGlyph() chooses that glyph from a quadrant coverage
-- mask rather than from brightness. That is why its pictures carry so much more
-- than an ASCII ramp does: the colours hold the levels, the glyph holds the
-- shape.
--
-- This panel has no CP437 - PicopixelFB is 0x20..0x7E and px.text() folds
-- lowercase onto uppercase, so there are no half-blocks and about 67 usable
-- shapes. Two things made it work anyway:
--
--   * px.rect() before px.text() is exactly the fg/bk pair. The background is
--     free.
--   * a cell here is 3x5 = 15 pixels, small enough that each glyph's whole
--     bitmap could be matched against the wanted ink instead of four quadrant
--     averages. So the choice is AverageGlyph's, made exactly.
--
-- Cells were fitted offline against the C2RMF scan of the painting; the table
-- below is the result. Every cell carries a glyph, including the flat ones -
-- asciicker never leaves one empty either, and a blank cell is what turns the
-- whole thing back into a colour mosaic.
--
-- The portrait is drawn once. The canvas is not cleared between frames on this
-- firmware, so only the clock half is repainted - 240 cells of painting cost
-- nothing after the first frame.

PERIOD = 60.0          -- px.t() is then the second hand, so the minute lands true
FPS = 4                -- nothing here moves faster than the seconds column

local ART_W, ART_H = 20, 12
local ART_G = {
  "L7^F,11597^L!,^^^^A$",
  "HF2,,, F^^^^,,PJ:JT,",
  "$^[ ,F,$$,,G`^,.,,,P",
  "E^V,F! * %J($[ .,C7+",
  ",7! 4'4} 4^%I (J,)$7",
  ") ,IJ++T/-+FI(  /7,^",
  " $ F / 4J,,FI!(J BL$",
  "')JY)I%C+,[[I(V)I!(+",
  ",,)(C^,$+^FJ!?!IJ(,)",
  "F,'.!' `!'*^$3!IBJ [",
  ":,,JY%' /`^F6.., $+4",
  "D`I'  ^ `$^^ IF DVI[",
}
local ART_B = {
  "A2B84AABBE50ACBE51AAC055A7BC44A9B73CADBB51B2C155A9BF53B0C86AB5C96AA3B64DA8BC57A3BE54AACB6FACC660ACBD55AFC15A6C7736A9BB4F",
  "C5D264C5D362BBC64BBBC646BBC84EBFC84FC9C94BA945049E3C017F28076E1C09812806B6C555B6C659BACE5FB8CB59B5C250B6BD44B8C761B2C361",
  "C9D368CDD566D1D85CD1D759CAD770A75302B45D02D67901B96002A24E067A2F0A411013370A152A0512D5DB52BFD169BBCA5FBBC85AB9CD6DC5D873",
  "82843FDAE37EDFDE69D7E3846D1004FBCB18FEF35BFEF14EFEF544FFE424F29F017A290A37081A2707251A0419D5E571C5D672D1DE72D8E276CFDC75",
  "DDDF79E8EB84F3F279DFD657740B18FEDA35FDE341FEF148FEED41FDC819FDCB21F3A80D4E0B1A2C08281C05201C0B1E9FB345AEBD51BED36896BF61",
  "E6EA77E5E67DE8EC83933B1B791624FFD128F5A20DFDC21AF19F0DF6B80DEB960FFCC71E661615330B352408332E0D305E80415D844C84B06368955C",
  "747534BEA22975763E8E1E136E0C17FEEF43FEE747FEEF50F7A81DFEF04DFEEB37DA7E05540D174314291B0C2D37102E5D743AA6B3545E8E55598852",
  "7B773C716E33666238651823520421FEB81CFEE62DBE4E06F4AC1BFED832EC960EB152094F0D1743152C25102D3112304A5B355B6A3B577A54527C54",
  "726E3A736F346E6D337F212367071F610E1FFBC821F08806FEC022F09D11BB5D07B2590F500F193D1025270D27301535592724656D38607C4F5B7C52",
  "8093597884499FAF5E7D311D670D1D540920520C1C6E221B9A42108D3C129D450B8D3E125715145011182E0F2231172F475A3E6A86445B7D4E587B49",
  "90985B8FA66B959E5684883C821B1F7812175E1E19682216FEC725EFAA15E1960CF0AB13B45F0578180A410C1A421220240727697A3A4A5A2759591C",
  "8C5B195F331F592F1A401334641123791E19FCC619FCD633FEE743FECE26FFED44FFEB3CFED31DA13608C35D0A913F0D5B1A253D0B26766826AC9B2D",
}
local ART_F = {
  "B2C4569BB03FA2B4409DB84DB4C84FB0C251AAB53FA9BC48AFC565AABC51A2BB58B2C35AA0B64AACC766A2BF53A8BE50A2B74EA7B848CADF65B2C15D",
  "BAC659BCC94EC2D163C8D251C4D05FCCD5618F2700C5CF59C3D15DBEBC49C0CA56C2CB4C611200D2E44FB5C858B3C557B4BD41B7C356B4BE4CBBCE69",
  "D3DB6EC6CD56C7D05FD7E07BD8ED8BD6DB5FF9C91EFEDB2BFBC614F6B002A34704642012801C03C8AE2A24070FC6DC73C1D97EC3D269C2D46DC0D26F",
  "FBFE7AD5D867D9DA61E1EB9CCECF66A54401FDD935FEF859FDE42EFEC40CC26802B15A0468150F3B0B20A46C0243240CBBCC60C7D569CFD766D6E47D",
  "EFF096DFE273DFE064A13B0AAB5A0DFCA401FEEE4DFDE845FFBE04FEDF2FF8A909CA7505A143045A0F1B390B28A1B8507B8532979F3995B95FC0D763",
  "90A34C8E862898B767B8AE47A44F20D37316BF520DFFF233BC5506C3560498390BB85E09C26402510F32430F2D5C5C407B963F95AC4B5E945891B862",
  "ACBC50827439C1C456A27524CB6501FFD519FBC117FCC826FFDE2DFED528FECE1EFDB510AC4903350F2E3A12301B0023A9B1516C823B99BA5E98C66D",
  "84813D80742F593D2B85291B87121AA43306FAAC12FED42FA43706E78C0BFEBA1ED17A0F8021053D102438142D180D364723264F58354C6845526A3E",
  "85934C8B974493964389491D861718F4A90C89260CFFE634DB770DFEC224EEA013903307771C084C16263F112A19092F6D7C367086428B923E7E8939",
  "6D6C449EAB607E7E3D8987429418138C1613B53C06FFD517F5A510C163066B27186520199244093D0B1D45182B220E2A3817295668389E963976934E",
  "87B489A580148E7F205E1224600E225E0C1A50111B9E5210DA9213B1620EAB590AC5770AE79C09BE5401731B1489340952482739251D9FA83C829446",
  "5E2B218D6C0D8C6A16724E13831D18FF9F01843613B3610FFDC226FFEA34FECC26FECF28FBAB0CF38E028D210E6B22163B0D2A5121264D2E1E5E5218",
}

-- ---------------------------------------------------------------- layout
local ART_X, ART_Y   = 1, 2      -- 20 x 12 cells of 3 x 5 = 60 x 60
local CW, CH         = 3, 5
local SEC_X          = 63        -- the seconds column, between the halves
local CLK_X, CLK_W   = 67, 61    -- everything to the right of it
local DCW, DCH       = 4, 5      -- a clock digit's cell: 3 px of ink and one of
                                 -- air, so the grid the characters sit in stays
                                 -- visible across a digit
local DGAP           = 7         -- between the two digits. It has to be wider
                                 -- than the gap inside a digit or the pair
                                 -- reads as one six-column block
local DIGIT_W        = 3 * DCW   -- 12
local DIGIT_H        = 5 * DCH   -- 25

-- 3x5, the smallest a digit reads at. Each 1 becomes one character on screen,
-- which is the whole point: the time is built out of type, like the face is.
local DIGITS = {
  [0] = {7,5,5,5,7}, [1] = {2,6,2,2,7}, [2] = {7,1,7,4,7}, [3] = {7,1,3,1,7},
  [4] = {5,5,7,1,1}, [5] = {7,4,7,1,7}, [6] = {7,4,7,5,7}, [7] = {7,1,1,1,1},
  [8] = {7,5,7,5,7}, [9] = {7,5,7,1,7},
}

-- Ink taken from the painting rather than chosen: the warm lead-tin yellow of
-- the face, and the umber it sits in.
local GOLD   = {236, 196, 112}
local GOLD_D = { 96,  70,  28}
local SHADOW = { 10,   9,  13}
local OCHRE  = {150, 116,  62}

-- The character laid over a lit cell. It has to be sparse: the gold is the
-- stroke, the glyph is only its grain. An "8" here takes back most of the
-- cell and the digit falls apart into a checkerboard.
local TEX = ":"

-- ---------------------------------------------------------- unpack at load
-- Done once, in the chunk body, which is what the 20M load budget is for.
local CELL = {}
for r = 1, ART_H do
  local g, b, f = ART_G[r], ART_B[r], ART_F[r]
  local row = {}
  for c = 1, ART_W do
    local h = (c - 1) * 6
    row[c] = {
      g:sub(c, c),
      tonumber(b:sub(h+1, h+2), 16), tonumber(b:sub(h+3, h+4), 16), tonumber(b:sub(h+5, h+6), 16),
      tonumber(f:sub(h+1, h+2), 16), tonumber(f:sub(h+3, h+4), 16), tonumber(f:sub(h+5, h+6), 16),
    }
  end
  CELL[r] = row
end
ART_G, ART_B, ART_F = nil, nil, nil

-- ------------------------------------------------------------------ paint
-- One cell, asciicker's way round: the background first, then the glyph over
-- it. px.text places a glyph's ink 2 rows below the y it is given, so the cell
-- at row y wants its text at y-2.
local function cell(x, y, ch, br, bg, bb, fr, fg, fb)
  px.rect(x, y, CW, CH, br, bg, bb, true)
  if ch ~= " " then px.text(x, y - 2, ch, fr, fg, fb) end
end

local function portrait()
  for r = 1, ART_H do
    local row, y = CELL[r], ART_Y + (r - 1) * CH
    for c = 1, ART_W do
      local u = row[c]
      cell(ART_X + (c - 1) * CW, y, u[1], u[2], u[3], u[4], u[5], u[6], u[7])
    end
  end
end

-- A lit cell is the painting's gold with a dark character laid over it, not a
-- bright character on black. That is asciicker's pair used the other way round,
-- and it is the only way a digit reads at this size: no ASCII glyph is solid,
-- so a digit built out of glyph-ink comes out as a scatter of dots. Put the
-- light in the background and the glyph becomes texture inside a shape that
-- holds together.
local function digit(n, x, y, r, g, b)
  local rowsOf = DIGITS[n]
  for ry = 1, 5 do
    local bits = rowsOf[ry]
    for rx = 1, 3 do
      -- bit 2 is the leftmost column, so 4, 2, 1 across
      if bits % (2 ^ (4 - rx)) >= (2 ^ (3 - rx)) then
        local cx, cy = x + (rx - 1) * DCW, y + (ry - 1) * DCH
        px.rect(cx, cy, 3, DCH, r, g, b, true)
        px.text(cx, cy - 2, TEX, SHADOW[1], SHADOW[2], SHADOW[3])
      end
    end
  end
end

local function pair(v, x, y, r, g, b)
  digit(math.floor(v / 10) % 10, x, y, r, g, b)
  digit(v % 10, x + DIGIT_W + DGAP, y, r, g, b)
end

local first = true

function draw()
  if first then
    px.clear(0, 0, 0)
    portrait()
    first = false
  end

  local t = px.now()
  local phase = px.t()

  -- Repaint only the right half; the painting stays where it was put.
  px.rect(SEC_X, 0, 128 - SEC_X, 64, SHADOW[1], SHADOW[2], SHADOW[3], true)

  local PAIR_W = DIGIT_W * 2 + DGAP
  local x = CLK_X + math.floor((CLK_W - PAIR_W) / 2)
  -- Hours above, minutes below, with a real gap between them: set any
  -- closer and the four digits read as one number.
  pair(t.hour, x, 3,  GOLD[1], GOLD[2], GOLD[3])
  pair(t.min,  x, 36, OCHRE[1] + 18, OCHRE[2] + 12, OCHRE[3] + 4)

  -- The seconds, as twelve cells filling up the divider: five seconds a cell,
  -- and the one in hand is brighter than the ones behind it.
  local lit = phase * ART_H
  for i = 1, ART_H do
    local y = ART_Y + (i - 1) * CH
    if i <= lit then
      if i > lit - 1 then
        -- the cell the minute is in: lit background, character on top
        px.rect(SEC_X, y, 3, CH, GOLD[1], GOLD[2], GOLD[3], true)
        px.text(SEC_X, y - 2, TEX, SHADOW[1], SHADOW[2], SHADOW[3])
      else
        px.rect(SEC_X, y, 3, CH, GOLD_D[1], GOLD_D[2], GOLD_D[3], true)
        px.text(SEC_X, y - 2, TEX, SHADOW[1], SHADOW[2], SHADOW[3])
      end
    else
      px.text(SEC_X, y - 2, ":", 52, 42, 28)
    end
  end
end
