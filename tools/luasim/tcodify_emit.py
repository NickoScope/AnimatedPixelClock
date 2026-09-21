import json, sys
d=json.load(open(sys.argv[1])); cells=d["cells"]
mk=[];bg=[];fg=[]
for row in cells:
    mk.append("".join(f"{c[0]:X}" for c in row))
    bg.append("".join(f"{v:02X}" for c in row for v in c[1]))
    fg.append("".join(f"{v:02X}" for c in row for v in c[2]))
def q(s): return '"'+s+'"'
print(f'local ART_W, ART_H = {d["cols"]}, {d["rows"]}')
for name, arr in (("ART_M",mk),("ART_B",bg),("ART_F",fg)):
    print(f"local {name} = {{\n  " + ",\n  ".join(q(r) for r in arr) + ",\n}")
