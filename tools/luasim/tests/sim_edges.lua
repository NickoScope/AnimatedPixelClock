-- px_sim.h at its edges, for fx_parity: fire, life and waves with parameters
-- out of range and NaN, odd rules, a wave with big drops; reaction-diffusion
-- seeded at the edges and off the canvas, stepped at the limits, params far
-- off, shown into a layer.
-- Not an effect: nothing here is meant to look like anything.
local nan = 0 / 0
local F, G, A, B, S = px.layer(), px.layer(), px.layer(128), px.layer(128), px.layer()
local R = px.reaction{ f = nan, k = 5, da = -1, db = 2 }
R:seed(0, 0, 5); R:seed(127, 63, 4); R:seed(-300, 900, 3); R:seed(64, 32, 0)
local pal = px.palette{ "cos", {0.5,0.5,0.5}, {0.5,0.5,0.5}, {1,1,1}, {0,0.33,0.67} }
local sd = 11
for y = 0, 63 do for x = 0, 127 do sd = (sd * 1103515245 + 12345) & 0x7fffffff; if sd % 100 < 35 then G:set(x, y, 255) end end end
local f = 0
function draw()
  f = f + 1
  local c = f % 5
  px.step(F, "fire", { cool = (c == 0) and 2 or 0.3, heat = (c == 1) and nan or 0.7, seed = f * 1e6 })
  px.step(G, "life", { decay = (c == 2) and 5 or 0.1, born = (c == 3) and "36" or "3", survive = (c == 4) and "" or "23" })
  if c == 0 then A:set(f % 128, (f * 7) % 64, 255); A:set(0, 0, 0) end
  px.step(A, "wave", { prev = B, damp = (c == 1) and 1 or 0.01 }); A, B = B, A
  if f == 20 then R:params{ f = 0.055, k = 0.062, da = 1, db = 0.5 } end
  if f == 40 then R:clear(); R:seed(64, 32, 8) end
  R:step((c == 2) and 32 or 3)
  R:show(S)
  local q = f % 4
  if q == 0 then px.show(F, pal) elseif q == 1 then px.show(G, pal) elseif q == 2 then px.show(A, pal) else px.show(S, pal) end
end
