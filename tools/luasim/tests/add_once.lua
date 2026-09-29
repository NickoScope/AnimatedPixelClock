-- px.mode("add") over circles and rects of every size: each pixel once, so
-- nothing on this canvas goes over 10 where the shapes do not overlap.
function draw()
  px.clear(0,0,0)
  px.mode("add")
  local f = math.floor(px.t() * 40)
  px.circle(20, 20, 17, 10, 10, 10, true)
  px.circle(60, 30, 20 + f % 5, 10, 10, 10)
  px.circle(100, 10, 0, 10, 10, 10)
  px.circle(100, 40, 1, 10, 10, 10, true)
  px.circle(110, 55, 25, 10, 10, 10, true)
  px.rect(40, 50, 30, 10, 10, 10, 10)
  px.rect(80, 2, 1, 10, 10, 10, 10)
  px.rect(85, 2, 10, 1, 10, 10, 10)
  px.rect(90, 20, 1, 1, 10, 10, 10)
  px.rect(-5, 60, 20, 8, 10, 10, 10)
end
