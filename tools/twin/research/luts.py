import re
src=open("/Users/apple/AnimatedPixelClock-netbroker/.pio/libdeps/matrix-waveshare-rgb/ESP32 HUB75 LED MATRIX PANEL DMA Display/src/cie_luts.h").read()
m=re.search(r"lumConvTab_8bit\[256\] = \{(.*?)\};",src,re.S)
tab=[int(x) for x in m.group(1).replace("\n"," ").split(",") if x.strip()]
def cie(L): return L/902.3 if L<=8 else ((L+16)/116)**3
calc=[round(cie(i/255*100)*255) for i in range(256)]
print(len(tab), tab==calc, [i for i in range(256) if tab[i]!=calc[i]][:10])
# some points
for v in [1,4,5,8,16,32,64,128,192,255]: print(v, tab[v])
