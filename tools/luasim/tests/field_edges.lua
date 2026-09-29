-- px_field.h at its edges, for fx_parity: every kind of term, huge and NaN
-- parameters, centres far off the canvas, 0 and 8 terms, bases out of range,
-- noise at big and negative coordinates.
-- Not an effect: nothing here is meant to look like anything.
local nan, inf = 0 / 0, 1 / 0
local L = px.layer()
local P = px.palette{ "cos", {0.5,0.5,0.5}, {0.5,0.5,0.5}, {1,1,1}, {0,0.33,0.67} }
local SETS = {
  {}, { {"sin", 0.1, 0.05} }, { {"sin", 1e9, -1e9, nan, 70} }, { {"ring", -2000, 5000, 1e6, nan, 1} },
  { {"ring"} }, { {"ray", 64, 32, 1000, 7, -3} }, { {"ray", nan, nan} }, { {"noise"} },
  { {"noise", 99, 1e30, 1, 99} }, { {"noise", -0.1, -40000, 2, 6} },
  { {"sin", 0.2, 0, 0, 0.3}, {"sin", 0, 0.2, 0, 0.3}, {"ring", 10, 10, 0.5, 0, 0.3}, {"ray", 100, 50, 3, 0, 0.3},
    {"noise", 0.05, 1, 0.3, 2}, {"sin", 0.01, 0.02, 1, 0.3}, {"ring", 120, 60, 0.2, 1, 0.3}, {"noise", 0.2, 3, 0.3, 1} },
}
local f = 0
function draw()
  f = f + 1
  local s = SETS[(f - 1) % #SETS + 1]
  local bases = { nil, 0, 255, -5000, 5000, nan }
  px.field(L, s, bases[f % #bases + 1])
  px.show(L, P, f)
  local n = px.noise(f * 1e4, -f * 3.3, inf)
  local m = px.noise(nan, 1e30)
  px.text(1, 1, string.format("%.3f %.3f", n, m), 255, 255, 255)
end
