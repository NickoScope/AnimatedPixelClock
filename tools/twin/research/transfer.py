from model import *
import re
src=open("/Users/apple/AnimatedPixelClock-netbroker/.pio/libdeps/matrix-waveshare-rgb/ESP32 HUB75 LED MATRIX PANEL DMA Display/src/cie_luts.h").read()
lut=[int(x) for x in re.search(r"lumConvTab_8bit\[256\] = \{(.*?)\};",src,re.S).group(1).replace("\n"," ").split(",") if x.strip()]
for brt in [255,50,128,10,1]:
    fb=clear_fb(); set_brightness(fb,brt)
    oe=oe_counts(fb)
    # displayed plane in segment s = plane of segment s-1 (row-wrap: seg0 shows previous row's last)
    w=[0]*DEPTH
    for s in range(len(seq)):
        prev = seq[s-1] if s>0 else seq[-1]
        w[prev]+=oe[seq[s]]
    total=len(seq)*W*RPF
    T=lambda code: sum(w[p] for p in range(DEPTH) if code>>p&1)
    nonmono=[(v,lut[v],lut[v+1]) for v in range(255) if T(lut[v+1])<T(lut[v])]
    print("brt",brt,"plane weights (PCLK clocks per row per frame):",w,"sum",sum(w),"max duty/LED %.4f"%(sum(w)/total))
    print("   8-bit input codes where next code is DIMMER:",nonmono[:12], "count",len(nonmono))
    raw=[(c) for c in range(255) if T(c+1)<T(c)]
    print("   raw 8-bit plane codes (after LUT) non-monotonic count:",len(raw), raw[:8])
