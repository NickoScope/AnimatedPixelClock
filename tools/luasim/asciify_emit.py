"""The cells, as a Lua table the effect can unpack at load."""
import json, sys
d = json.load(open(sys.argv[1]))
cols, rows, cells = d["cols"], d["rows"], d["cells"]
gl, bg, fg = [], [], []
for row in cells:
    gl.append("".join(c[2] for c in row))
    bg.append("".join(f"{v:02X}" for c in row for v in c[0]))
    fg.append("".join(f"{v:02X}" for c in row for v in c[1]))
def q(s):  # the palette drops " and \, so a double-quoted Lua string is always safe
    assert '"' not in s and "\\" not in s, s
    return '"' + s + '"' 
print(f"local ART_W, ART_H = {cols}, {rows}")
print("local ART_G = {\n  " + ",\n  ".join(q(r) for r in gl) + ",\n}")
print("local ART_B = {\n  " + ",\n  ".join(q(r) for r in bg) + ",\n}")
print("local ART_F = {\n  " + ",\n  ".join(q(r) for r in fg) + ",\n}")
