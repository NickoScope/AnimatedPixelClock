"""An image into quadrant cells, by libtcod's rule. See quad.py."""
import argparse, json
from PIL import Image, ImageEnhance
from quad import quadrant_cell

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image"); ap.add_argument("--cols", type=int, default=20)
    ap.add_argument("--rows", type=int, default=16)
    ap.add_argument("--cw", type=int, default=4); ap.add_argument("--chh", type=int, default=4)
    ap.add_argument("--crop", default=""); ap.add_argument("--contrast", type=float, default=1.0)
    ap.add_argument("--saturation", type=float, default=1.0); ap.add_argument("--gamma", type=float, default=1.0)
    ap.add_argument("--out", default="q.json"); ap.add_argument("--preview", default="")
    a = ap.parse_args()

    im = Image.open(a.image).convert("RGB")
    if a.crop:
        l,t,r,b = (float(v) for v in a.crop.split(",")); W,H = im.size
        im = im.crop((int(l*W), int(t*H), int(r*W), int(b*H)))
    if a.gamma != 1.0:
        g=a.gamma; im = im.point(lambda v: int(255*((v/255)**(1/g))))
    if a.contrast != 1.0: im = ImageEnhance.Contrast(im).enhance(a.contrast)
    if a.saturation != 1.0: im = ImageEnhance.Color(im).enhance(a.saturation)

    im = im.resize((a.cols*2, a.rows*2), Image.LANCZOS)
    p = im.load()
    art = []
    for cy in range(a.rows):
        row = []
        for cx in range(a.cols):
            tl, tr = p[cx*2, cy*2], p[cx*2+1, cy*2]
            bl, br = p[cx*2, cy*2+1], p[cx*2+1, cy*2+1]
            fg_set, bg, fg = quadrant_cell((tl, tr, bl, br))
            # mask as a nibble over (TL,TR,BL,BR) for the emitter
            m = 0
            for i in fg_set: m |= 1 << i
            row.append((m, bg, fg))
        art.append(row)
    json.dump({"cols":a.cols,"rows":a.rows,"cw":a.cw,"ch":a.chh,
               "cells":[[[m,list(bg),list(fg)] for m,bg,fg in r] for r in art]}, open(a.out,"w"))
    from collections import Counter
    c = Counter(m for r in art for m,_,_ in r)
    print(f"{a.cols}x{a.rows}, маски: " + ", ".join(f"{k:X}:{v}" for k,v in c.most_common()))

    if a.preview:
        out = Image.new("RGB",(a.cols*a.cw, a.rows*a.chh)); q = out.load()
        hw, hh = a.cw//2, a.chh//2
        boxes = [(0,0,hw,hh),(hw,0,a.cw-hw,hh),(0,hh,hw,a.chh-hh),(hw,hh,a.cw-hw,a.chh-hh)]
        for cy,row in enumerate(art):
            for cx,(m,bg,fg) in enumerate(row):
                for i,(ox,oy,w,h) in enumerate(boxes):
                    col = fg if (m>>i)&1 else bg
                    for yy in range(h):
                        for xx in range(w):
                            q[cx*a.cw+ox+xx, cy*a.chh+oy+yy] = col
        out.resize((out.width*8,out.height*8), Image.NEAREST).save(a.preview)
        print("превью:", a.preview)

main()
